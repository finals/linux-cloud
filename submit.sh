#!/bin/bash 
#
# 将目标kernel代码，拷贝回本地仓库
#

# 拷贝目录函数
function _copy_dir() {
    if test $# -ne 2
    then
        echo "Usage: _copy_dir [source dir] [destination dir]";
        exit -1;
    fi

    src_dir=$1;
    dst_dir=$2;
    
    if test ! -d $src_dir
    then
        echo "$src_dir is not exist.";
        exit -1;
    fi

    if test -d $dst_dir
    then
        read -p $"$dst_dir already exists, remove? (yes or no):" remove
        if test $remove = "yes"
        then 
            rm -rf $dst_dir;
            echo "removed $dst_dir.";
        else
            exit 0;
        fi
    fi

    dst_dir=${dst_dir%/*}

    mkdir -p $dst_dir;    
    cp -rf $src_dir $dst_dir;
}

#拷贝文件函数
function _copy_file() {
    if test $# -ne 2
    then
        echo "Usage: _copy_file [source file] [destination file]";
        exit -1;
    fi
  
    src_file=$1;
    dst_file=$2;

    if test ! -f $src_file
    then
        echo "$src_file is not exist.";
        exit -1;
    fi

    if test -f $dst_file
    then
        read -p $"$dst_file already exists, remove? (yes or no)" remove
        if test $remove = "yes"
        then
            rm -rf $dst_file;
            echo "removed $dst_file.";
        else
            exit 0;
        fi
    fi

    mkdir -p ${dst_file%/*}
    cp -f $src_file $dst_file;
}

# 拷贝bcache到指定目录
function submit_bcache_module() {
    bcache_module_dst_dir="drivers/md/bcache";
    bcache_module_src_dir="$1/$bcache_module_dst_dir";
    _copy_dir $bcache_module_src_dir $bcache_module_dst_dir

    #清理编译的文件
    rm -f $bcache_module_dst_dir/*.o $bcache_module_dst_dir/*.cmd $bcache_module_dst_dir/*.mod 
    rm -f $bcache_module_dst_dir/*.mod.c $bcache_module_dst_dir/*.order 
    rm -f $bcache_module_dst_dir/.*.cmd $bcache_module_dst_dir/.*.d 
    rm -f $bcache_module_dst_dir/*.ko    

    #清理bcache中的.git
    if test -d "$bcache_module_dst_dir/.git"
    then
        rm -rf $bcache_module_dst_dir/.git
    fi
}

function submit_bcache_api() {
   bcache_api_dst_file="include/uapi/linux/bcache.h";
   bcache_api_src_file="$1/$bcache_api_dst_file";
   _copy_file $bcache_api_src_file $bcache_api_dst_file
}

function submit_bcache_trace_event() {
    bcache_trace_event_dst_file="include/trace/events/bcache.h";
    bcache_trace_event_src_file="$1/$bcache_trace_event_dst_file";
    _copy_file $bcache_trace_event_src_file $bcache_trace_event_dst_file
}

function submit_bcache() {
    # TODO 检查是否是Linux源码目录
    submit_bcache_module $1;
    submit_bcache_api $1;  
    submit_bcache_trace_event $1;
}


function main() {
    if test $# -ne 2
    then
        echo "Usage: ./apply.sh [module] [destination path]";
        exit -1;
    fi

    if test ! -e $2 
    then
        echo "$2 is not exits.";
        exit -1;
    fi

    if test $1 = "bcache" -o $1 = "all"
    then 
        submit_bcache $2
    fi
}

main $1 $2
