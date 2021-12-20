#!/bin/bash
#
# This file is used to install jh_taos client on linux systems. The operating system
# is required to use systemd to manage services at boot

set -e
#set -x

# -----------------------Variables definition---------------------

osType=Linux
pagMode=full

if [ "$osType" != "Darwin" ]; then
    script_dir=$(dirname $(readlink -f "$0"))
    # Dynamic directory
    data_dir="/var/lib/jh_taos"
    log_dir="/var/log/jh_taos"
else
    script_dir=`dirname $0`
    cd ${script_dir}
    script_dir="$(pwd)"
    data_dir="/var/lib/jh_taos"
    log_dir="~/jh_taos/log"
fi

log_link_dir="/usr/local/jh_taos/log"

cfg_install_dir="/etc/jh_taos"

if [ "$osType" != "Darwin" ]; then
    bin_link_dir="/usr/bin"
    lib_link_dir="/usr/lib"
    lib64_link_dir="/usr/lib64"
    inc_link_dir="/usr/include"
else
    bin_link_dir="/usr/local/bin"
    lib_link_dir="/usr/local/lib"
    inc_link_dir="/usr/local/include"
fi

#install main path
install_main_dir="/usr/local/jh_taos"

# old bin dir
bin_dir="/usr/local/jh_taos/bin"

# Color setting
RED='\033[0;31m'
GREEN='\033[1;32m'
GREEN_DARK='\033[0;32m'
GREEN_UNDERLINE='\033[4;32m'
NC='\033[0m'

csudo=""
if command -v sudo > /dev/null; then
    csudo="sudo "
fi

update_flag=0

function kill_client() {
  pid=$(ps -ef | grep "jh_taos" | grep -v "grep" | awk '{print $2}')
  if [ -n "$pid" ]; then
    ${csudo}kill -9 $pid   || :
  fi
}

function install_main_path() {
    #create install main dir and all sub dir
    ${csudo}rm -rf ${install_main_dir}    || :
    ${csudo}mkdir -p ${install_main_dir}
    ${csudo}mkdir -p ${install_main_dir}/cfg
    ${csudo}mkdir -p ${install_main_dir}/bin
    ${csudo}mkdir -p ${install_main_dir}/connector
    ${csudo}mkdir -p ${install_main_dir}/driver
    ${csudo}mkdir -p ${install_main_dir}/examples
    ${csudo}mkdir -p ${install_main_dir}/include
}

function install_bin() {
  # Remove links
  ${csudo}rm -f ${bin_link_dir}/jh_taos         || :
  if [ "$osType" != "Darwin" ]; then
      ${csudo}rm -f ${bin_link_dir}/jhdemo  || :
      ${csudo}rm -f ${bin_link_dir}/jh_taosdump  || :
  fi
  ${csudo}rm -f ${bin_link_dir}/rmjh       || :
  ${csudo}rm -f ${bin_link_dir}/set_core      || :

  ${csudo}cp -r ${script_dir}/bin/* ${install_main_dir}/bin && ${csudo}chmod 0555 ${install_main_dir}/bin/*

  #Make link
  [ -x ${install_main_dir}/bin/jh_taos ] && ${csudo}ln -s ${install_main_dir}/bin/jh_taos ${bin_link_dir}/jh_taos    || :
  if [ "$osType" != "Darwin" ]; then
      [ -x ${install_main_dir}/bin/jhdemo ] && ${csudo}ln -s ${install_main_dir}/bin/jhdemo ${bin_link_dir}/jhdemo || :
      [ -x ${install_main_dir}/bin/jh_taosdump ] && ${csudo}ln -s ${install_main_dir}/bin/jh_taosdump ${bin_link_dir}/jh_taosdump || :
  fi
  [ -x ${install_main_dir}/bin/remove_client_jh.sh ] && ${csudo}ln -s ${install_main_dir}/bin/remove_client_jh.sh ${bin_link_dir}/rmjh || :
  [ -x ${install_main_dir}/bin/set_core.sh ] && ${csudo}ln -s ${install_main_dir}/bin/set_core.sh ${bin_link_dir}/set_core || :
}

function clean_lib() {
    sudo rm -f /usr/lib/libtaos.* || :
    sudo rm -rf ${lib_dir} || :
}

function install_lib() {
    # Remove links
    ${csudo}rm -f ${lib_link_dir}/libtaos.*         || :
    ${csudo}rm -f ${lib64_link_dir}/libtaos.*       || :

    ${csudo}cp -rf ${script_dir}/driver/* ${install_main_dir}/driver && ${csudo}chmod 777 ${install_main_dir}/driver/*

    if [ "$osType" != "Darwin" ]; then
        ${csudo}ln -s ${install_main_dir}/driver/libtaos.* ${lib_link_dir}/libtaos.so.1
        ${csudo}ln -s ${lib_link_dir}/libtaos.so.1 ${lib_link_dir}/libtaos.so
        
        if [ -d "${lib64_link_dir}" ]; then
	        ${csudo}ln -s ${install_main_dir}/driver/libtaos.* ${lib64_link_dir}/libtaos.so.1       || :
	        ${csudo}ln -s ${lib64_link_dir}/libtaos.so.1 ${lib64_link_dir}/libtaos.so               || :
	      fi
    else
        ${csudo}ln -s ${install_main_dir}/driver/libtaos.* ${lib_link_dir}/libtaos.1.dylib
        ${csudo}ln -s ${lib_link_dir}/libtaos.1.dylib ${lib_link_dir}/libtaos.dylib
    fi
    
    ${csudo}ldconfig
}

function install_header() {
    ${csudo}rm -f ${inc_link_dir}/taos.h ${inc_link_dir}/taoserror.h    || :
    ${csudo}cp -f ${script_dir}/inc/* ${install_main_dir}/include && ${csudo}chmod 644 ${install_main_dir}/include/*
    ${csudo}ln -s ${install_main_dir}/include/taos.h ${inc_link_dir}/taos.h
    ${csudo}ln -s ${install_main_dir}/include/taoserror.h ${inc_link_dir}/taoserror.h
}

function install_config() {
    if [ ! -f ${cfg_install_dir}/taos.cfg ]; then
        ${csudo}mkdir -p ${cfg_install_dir}
        [ -f ${script_dir}/cfg/taos.cfg ] && ${csudo}cp ${script_dir}/cfg/taos.cfg ${cfg_install_dir}
        ${csudo}chmod 644 ${cfg_install_dir}/*
    fi

    ${csudo}cp -f ${script_dir}/cfg/taos.cfg ${install_main_dir}/cfg/taos.cfg.org
    ${csudo}ln -s ${cfg_install_dir}/taos.cfg ${install_main_dir}/cfg
}


function install_log() {
    ${csudo}rm -rf ${log_dir}  || :

    if [ "$osType" != "Darwin" ]; then
        ${csudo}mkdir -p ${log_dir} && ${csudo}chmod 777 ${log_dir}
    else
        mkdir -p ${log_dir} && ${csudo}chmod 777 ${log_dir}
    fi
    ${csudo}ln -s ${log_dir} ${install_main_dir}/log
}

function install_connector() {
    ${csudo}cp -rf ${script_dir}/connector/* ${install_main_dir}/connector
}

function install_examples() {
    if [ -d ${script_dir}/examples ]; then
        ${csudo}cp -rf ${script_dir}/examples/* ${install_main_dir}/examples
    fi
}

function update() {
    # Start to update
    if [ ! -e jh_taos.tar.gz ]; then
        echo "File jh_taos.tar.gz does not exist"
        exit 1
    fi
    tar -zxf jh_taos.tar.gz

    echo -e "${GREEN}Start to update jh_iot client...${NC}"
    # Stop the client shell if running
    if pidof jh_taos &> /dev/null; then
        kill_client
        sleep 1
    fi

    install_main_path

    install_log
    install_header
    install_lib
    install_examples
    install_bin
    install_config

    echo
    echo -e "\033[44;32;1mjh_iot client is updated successfully!${NC}"

    rm -rf $(tar -tf jh_taos.tar.gz)
}

function install() {
    # Start to install
    if [ ! -e jh_taos.tar.gz ]; then
        echo "File jh_taos.tar.gz does not exist"
        exit 1
    fi
    tar -zxf jh_taos.tar.gz

    echo -e "${GREEN}Start to install jh_taos client...${NC}"

	install_main_path
    install_log
    install_header
    install_lib
    install_examples
    install_bin
    install_config

    echo
    echo -e "\033[44;32;1mjh_iot client is installed successfully!${NC}"

    rm -rf $(tar -tf jh_taos.tar.gz)
}


## ==============================Main program starts from here============================
# Install or updata client and client
# if server is already install, don't install client
  if [ -e ${bin_dir}/jh_taosd ]; then
      echo -e "\033[44;32;1mThere are already installed jh_iot server, so don't need install client!${NC}"
      exit 0
  fi

  if [ -x ${bin_dir}/jh_taos ]; then
      update_flag=1
      update
  else
      install
  fi
