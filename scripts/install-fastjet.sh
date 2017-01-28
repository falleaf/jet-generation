#!/usr/bin/env bash

# Installs FastJet - this should *always* be run as root

if [ "$EUID" -ne 0 ]
  then echo "*** Please run as root ***"
  exit 1
fi

if [[ ! -z $1  ]]; then
	echo "setting -j$1 as a flag to make invocations"
	MAKEFLAGS="-j$1"
fi

echo "copying .bashrc to .bashrc-backup-pythia for safe keeping"
cp ~/.bashrc ~/.bashrc-backup-pythia

mkdir -p /opt

pushd /opt

wget -q http://fastjet.fr/repo/fastjet-3.2.1.tar.gz
tar -xzf fastjet-3.2.1.tar.gz

if [[ ! -z $USER  ]]; then
	echo "running a chown op on /opt/fastjet-3.2.1"
	chown -R $USER /opt/fastjet-3.2.1 
fi

cd fastjet-3.2.1        && \
    ./configure         && \
    make $MAKEFLAGS     && \
    make install        && \
    cd /opt

popd