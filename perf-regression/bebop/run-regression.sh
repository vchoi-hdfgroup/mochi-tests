#!/bin/bash

# This is a shell script to be run from a login node of the Bebop system at
# the LCRC, that will download, compile, and execute the ssg performance 
# regression tests, including any dependencies

# SEE README.spack.md for environment setup information!  This script will not
#     work properly without properly configured spack environment

# exit on any error
set -e

get_psm2_lib_path() {
    module show `spack module tcl find opa-psm2` |&grep LD_LIBRARY_PATH | cut -d \" -f 4
}

# load newer gcc up front
module load gcc_new/7.3.0

# location of this script
ORIGIN=$PWD
# scratch area for builds
SANDBOX=$PWD/mochi-regression-sandbox-$$
# install destination
PREFIX=$PWD/mochi-regression-install-$$
# job submission dir
JOBDIR=$PWD/mochi-regression-job-$$
# modify HOME env variable so that we don't perturb ~/.spack/ files for the 
# users calling this script
export HOME=$SANDBOX

mkdir $SANDBOX
mkdir $PREFIX
mkdir $JOBDIR
cp $ORIGIN/margo-regression.sbatch $JOBDIR
cp $ORIGIN/bake-regression.sbatch $JOBDIR
cp $ORIGIN/pmdk-regression.sbatch $JOBDIR
cp $ORIGIN/mobject-regression.sbatch $JOBDIR

# set up build environment
cd $SANDBOX
# as of 2019-05-17, clone fork with correction to PSM2 package
git clone -b carns/dev-opa-psm2-path https://github.com/carns/spack.git
git clone https://xgitlab.cels.anl.gov/sds/sds-repo.git
git clone https://xgitlab.cels.anl.gov/sds/sds-tests.git

echo "=== BUILD SPACK PACKAGES AND LOAD ==="
. $SANDBOX/spack/share/spack/setup-env.sh
spack compiler find
spack compilers

# use our own packages.yaml for bebop-specific preferences
cp $ORIGIN/packages.yaml $SPACK_ROOT/etc/spack
# add external repo for mochi.  Note that this will not modify the 
# user's ~/.spack/ files because we modified $HOME above
spack repo add ${SANDBOX}/sds-repo
# sanity check
spack repo list
# clean out any stray packages from previous runs, just in case
spack uninstall -R -y argobots mercury opa-psm2 bake || true

# nightly tests should test nightly software!
# spack install ior@develop+mobject ^margo@develop ^mercury@develop ^mobject@develop ^bake@develop ^remi@develop ^thallium@develop ^sdskeyval@develop ^ssg@develop

# ior acts as our "apex" package here, causing several other packages to build
spack install ior@develop +mobject 
# deliberately repeat setup-env step after building modules to ensure
#   that we pick up the right module paths
. $SANDBOX/spack/share/spack/setup-env.sh
# load ssg and bake because they are needed by things compiled outside of
# spack later in this script
spack load -r ssg
spack load -r bake

# sds-tests
echo "=== BUILDING SDS TEST PROGRAMS ==="
# TODO: why is this needed?  For some reason when we link software
#       outside of spack we are not picking up the path from the psm2 package
LIB_PATH_HACK=$(get_psm2_lib_path)
cd $SANDBOX/sds-tests
./prepare.sh
mkdir build
cd build
echo ../configure --prefix=$PREFIX CC=mpicc LDFLAGS="-L$LIB_PATH_HACK"
../configure --prefix=$PREFIX CC=mpicc LDFLAGS="-L$LIB_PATH_HACK"
make -j 3
make install

# set up job to run
echo "=== SUBMITTING AND WAITING FOR JOB ==="
cp $PREFIX/bin/margo-p2p-latency $JOBDIR
cp $PREFIX/bin/margo-p2p-bw $JOBDIR
cp $PREFIX/bin/bake-p2p-bw $JOBDIR
cp $PREFIX/bin/pmdk-bw $JOBDIR
cd $JOBDIR

export SANDBOX
sbatch --wait --export=ALL ./margo-regression.sbatch
sbatch --wait --export=ALL ./bake-regression.sbatch
sbatch --wait --export=ALL ./pmdk-regression.sbatch
sbatch --wait --export=ALL ./mobject-regression.sbatch

echo "=== JOB DONE, COLLECTING AND SENDING RESULTS ==="
# gather output, strip out funny characters, mail
cat *.out > combined.$JOBID.txt
# dos2unix combined.$JOBID.txt
mailx -s "mochi-regression (bebop)" sds-commits@lists.mcs.anl.gov < combined.$JOBID.txt

cd /tmp
rm -rf $SANDBOX
rm -rf $PREFIX
