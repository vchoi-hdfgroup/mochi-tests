/*
 * Copyright (c) 2018 UChicago Argonne, LLC
 *
 * See COPYRIGHT in top-level directory.
 */

/* Effective streaming bandwidth test between a single bake server and a
 * single bake client.
 */
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <mpi.h>

#include <margo.h>
#include <mercury.h>
#include <abt.h>
#include <ssg.h>
#include <ssg-mpi.h>
#include <bake-server.h>
#include <bake-client.h>

struct options
{
    unsigned long xfer_size;
    unsigned long total_mem_size;
    int duration_seconds;
    int concurrency;
    unsigned int mercury_timeout_client;
    unsigned int mercury_timeout_server;
    char* diag_file_name;
    char* na_transport;
    char* bake_pool;
};

struct bench_worker_arg
{
    bake_provider_handle_t bph;
    bake_target_id_t bti;
    ABT_mutex *cur_off_mutex;
    unsigned long *cur_off;
};

/* defealt to 512 MiB total xfer unless specified otherwise */
#define DEF_BW_TOTAL_MEM_SIZE 524288000UL
/* defealt to 1 MiB xfer sizes unless specified otherwise */
#define DEF_BW_XFER_SIZE 1048576UL

static int parse_args(int argc, char **argv, struct options *opts);
static void usage(void);

static struct options g_opts;
static char *g_buffer = NULL;

DECLARE_MARGO_RPC_HANDLER(bench_stop_ult);
static hg_id_t bench_stop_id;
static ABT_eventual bench_stop_eventual;
static int run_benchmark(struct options *opts, bake_provider_handle_t bph, 
    bake_target_id_t bti);
static void bench_worker(void *_arg);

int main(int argc, char **argv) 
{
    margo_instance_id mid;
    int nranks;
    int ret;
    ssg_group_id_t gid;
    ssg_member_id_t self;
    int rank;
    int namelen;
    char processor_name[MPI_MAX_PROCESSOR_NAME];
    struct hg_init_info hii;

    MPI_Init(&argc, &argv);

    /* TODO: relax this, maybe 1 server N clients? */
    /* 2 processes only */
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);
    if(nranks != 2)
    {
        usage();
        exit(EXIT_FAILURE);
    }
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Get_processor_name(processor_name,&namelen);
    printf("Process %d of %d is on %s\n",
	rank, nranks, processor_name);

    ret = parse_args(argc, argv, &g_opts);
    if(ret < 0)
    {
        if(rank == 0)
            usage();
        exit(EXIT_FAILURE);
    }

    /* allocate one big buffer for writes on client */
    if(rank > 0)
    {
        g_buffer = calloc(g_opts.total_mem_size, 1);
        if(!g_buffer)
        {
            fprintf(stderr, "Error: unable to allocate %lu byte buffer.\n", g_opts.total_mem_size);
            return(-1);
        }
    }

    memset(&hii, 0, sizeof(hii));
    if((rank > 0 && g_opts.mercury_timeout_client == 0) ||
       (rank == 0 && g_opts.mercury_timeout_server == 0))
    {
        
        /* If mercury timeout of zero is requested, then set
         * init option to NO_BLOCK.  This allows some transports to go
         * faster because they do not have to set up or maintain the data
         * structures necessary for signaling completion on blocked
         * operations.
         */
        hii.na_init_info.progress_mode = NA_NO_BLOCK;
    }

    /* actually start margo */
    mid = margo_init_opt(g_opts.na_transport, MARGO_SERVER_MODE, &hii, 0, -1);
    assert(mid);

    if(g_opts.diag_file_name)
        margo_diag_start(mid);

    /* adjust mercury timeout in Margo if requested */
    if(rank > 0 && g_opts.mercury_timeout_client != UINT_MAX)
        margo_set_param(mid, MARGO_PARAM_PROGRESS_TIMEOUT_UB, &g_opts.mercury_timeout_client);
    if(rank == 0 && g_opts.mercury_timeout_server != UINT_MAX)
        margo_set_param(mid, MARGO_PARAM_PROGRESS_TIMEOUT_UB, &g_opts.mercury_timeout_server);

    bench_stop_id = MARGO_REGISTER(
        mid, 
        "bench_stop_rpc", 
        void,
        void,
        bench_stop_ult);

    /* set up group */
    ret = ssg_init(mid);
    assert(ret == 0);
    gid = ssg_group_create_mpi("bake-bench", MPI_COMM_WORLD, NULL, NULL);
    assert(gid != SSG_GROUP_ID_NULL);

    assert(ssg_get_group_size(gid) == 2);

    self = ssg_get_group_self_id(gid);

    if(self == 0)
    {
        bake_provider_t provider;
        bake_target_id_t tid;

        /* server side */

        ret = bake_provider_register(mid, 1, BAKE_ABT_POOL_DEFAULT, &provider);
        assert(ret == 0);

        ret = bake_provider_add_storage_target(provider, g_opts.bake_pool, &tid);
        if(ret != 0)
        {
            fprintf(stderr, "Error: failed to add bake pool %s\n", g_opts.bake_pool);
            abort();
        }

        ret = ABT_eventual_create(0, &bench_stop_eventual);
        assert(ret == 0);
    }

    MPI_Barrier(MPI_COMM_WORLD);

    if(self > 0)
    {
        /* ssg id 1 (client) initiates benchmark */
        hg_handle_t handle;
        hg_addr_t target_addr;
        bake_client_t bcl;
        bake_provider_handle_t bph;
        bake_target_id_t bti;
        uint64_t num_targets = 0;

        target_addr = ssg_get_addr(gid, 0);
        assert(target_addr != HG_ADDR_NULL);

        ret = bake_client_init(mid, &bcl);
        assert(ret == 0);

        ret = bake_provider_handle_create(bcl, target_addr, 1, &bph);
        assert(ret == 0);

        ret = bake_probe(bph, 1, &bti, &num_targets);
        assert(ret == 0 && num_targets == 1);

        ret = run_benchmark(&g_opts, bph, bti);
        assert(ret == 0);

        bake_provider_handle_release(bph);
        bake_client_finalize(bcl);

        /* tell the server we are done */
        ret = margo_create(mid, target_addr, bench_stop_id, &handle);
        assert(ret == 0);
        ret = margo_forward(handle, NULL);
        assert(ret == 0);
        margo_destroy(handle);
    }
    else
    {
        /* ssg id 0 (server) services requests until told to stop */
        ABT_eventual_wait(bench_stop_eventual, NULL);
        sleep(3);
    }

    ssg_group_destroy(gid);
    ssg_finalize();

    if(g_opts.diag_file_name)
        margo_diag_dump(mid, g_opts.diag_file_name, 1);

    if(rank == 0)
        free(g_buffer);

    margo_finalize(mid);
    MPI_Finalize();

    return 0;
}

static int parse_args(int argc, char **argv, struct options *opts)
{
    int opt;
    int ret;

    memset(opts, 0, sizeof(*opts));

    opts->concurrency = 1;
    opts->total_mem_size = DEF_BW_TOTAL_MEM_SIZE;
    opts->xfer_size = DEF_BW_XFER_SIZE;

    /* default to using whatever the standard timeout is in margo */
    opts->mercury_timeout_client = UINT_MAX;
    opts->mercury_timeout_server = UINT_MAX; 

    while((opt = getopt(argc, argv, "n:x:c:d:t:p:m:")) != -1)
    {
        switch(opt)
        {
            case 'p':
                opts->bake_pool = strdup(optarg);
                if(!opts->bake_pool)
                {
                    perror("strdup");
                    return(-1);
                }
                break;
            case 'd':
                opts->diag_file_name = strdup(optarg);
                if(!opts->diag_file_name)
                {
                    perror("strdup");
                    return(-1);
                }
                break;
            case 'x':
                ret = sscanf(optarg, "%lu", &opts->xfer_size);
                if(ret != 1)
                    return(-1);
                break;
            case 'm':
                ret = sscanf(optarg, "%lu", &opts->total_mem_size);
                if(ret != 1)
                    return(-1);
                break;
            case 'c':
                ret = sscanf(optarg, "%d", &opts->concurrency);
                if(ret != 1)
                    return(-1);
                break;
            case 't':
                ret = sscanf(optarg, "%u,%u", &opts->mercury_timeout_client, &opts->mercury_timeout_server);
                if(ret != 2)
                    return(-1);
                break;
            case 'n':
                opts->na_transport = strdup(optarg);
                if(!opts->na_transport)
                {
                    perror("strdup");
                    return(-1);
                }
                break;
            default:
                return(-1);
        }
    }

    if(opts->concurrency < 1 || !opts->na_transport 
     || !opts->bake_pool)
    {
        return(-1);
    }

    return(0);
}

static void usage(void)
{
    fprintf(stderr,
        "Usage: "
        "bake-p2p-bw -x <xfer_size> -m <total_mem_size> -n <na>\n"
        "\t-x <xfer_size> - size of each bulk tranfer in bytes\n"
        "\t-m <total_mem_size> - total amount of data to write from each client process\n"
        "\t-n <na> - na transport\n"
        "\t-p <bake pool> - existing pool created with bake-mkpool\n"
        "\t[-c concurrency] - number of concurrent operations to issue with ULTs\n"
        "\t[-d filename] - enable diagnostics output\n"
        "\t[-t client_progress_timeout,server_progress_timeout] # use \"-t 0,0\" to busy spin\n"
        "\t\texample: mpiexec -n 2 ./bake-p2p-bw -x 4096 -n verbs://\n"
        "\t\t(must be run with exactly 2 processes\n");
    
    return;
}

/* tell server process that the benchmark is done */
static void bench_stop_ult(hg_handle_t handle)
{
    margo_respond(handle, NULL);
    margo_destroy(handle);

    ABT_eventual_set(bench_stop_eventual, NULL, 0);

    return;
}
DEFINE_MARGO_RPC_HANDLER(bench_stop_ult)


static int run_benchmark(struct options *opts, bake_provider_handle_t bph, 
    bake_target_id_t bti)
{
    ABT_pool pool;
    ABT_xstream xstream;
    int ret;
    int i;
    ABT_thread *tid_array;
    struct bench_worker_arg *arg_array;
    ABT_mutex cur_off_mutex;
    unsigned long cur_off = 0;
    double start_tm, end_tm;

    tid_array = malloc(g_opts.concurrency * sizeof(*tid_array));
    assert(tid_array);
    arg_array = malloc(g_opts.concurrency * sizeof(*arg_array));
    assert(arg_array);

    ABT_mutex_create(&cur_off_mutex);

    ret = ABT_xstream_self(&xstream);
    assert(ret == 0);

    ret = ABT_xstream_get_main_pools(xstream, 1, &pool);
    assert(ret == 0);

    start_tm = ABT_get_wtime();
    for(i=0; i<g_opts.concurrency; i++)
    {
        arg_array[i].bph = bph;
        arg_array[i].bti = bti;
        arg_array[i].cur_off_mutex = &cur_off_mutex;
        arg_array[i].cur_off = &cur_off;
        ret = ABT_thread_create(pool, bench_worker, 
            &arg_array[i], ABT_THREAD_ATTR_NULL, &tid_array[i]);
        assert(ret == 0);
    }

    for(i=0; i<g_opts.concurrency; i++)
    {
        ABT_thread_join(tid_array[i]);
        ABT_thread_free(&tid_array[i]);
    }
    end_tm = ABT_get_wtime();

    printf("<op>\t<concurrency>\t<xfer_size>\t<total_bytes>\t<seconds>\t<MiB/s>\n");
    printf("create_write_persist\t%d\t%lu\t%lu\t%f\t%f\n",
        g_opts.concurrency,
        g_opts.xfer_size,
        g_opts.total_mem_size,
        (end_tm-start_tm),
        ((double)g_opts.total_mem_size/(end_tm-start_tm))/(1024.0*1024.0));

    free(tid_array);
    ABT_mutex_free(&cur_off_mutex);

    return(0);
}

static void bench_worker(void *_arg)
{
    struct bench_worker_arg* arg = _arg;
    bake_region_id_t rid;
    int ret;
    char* this_buffer;

    ABT_mutex_spinlock(*arg->cur_off_mutex);
    while(*arg->cur_off < g_opts.total_mem_size)
    {
        this_buffer  = (char*)((unsigned long)g_buffer + *arg->cur_off);
        (*arg->cur_off) += g_opts.xfer_size;
        ABT_mutex_unlock(*arg->cur_off_mutex);

        ret = bake_create_write_persist(arg->bph, arg->bti, this_buffer, 
            g_opts.xfer_size, &rid);
        assert(ret == 0);
        ABT_mutex_spinlock(*arg->cur_off_mutex);
    }

    ABT_mutex_unlock(*arg->cur_off_mutex);

    return;
}