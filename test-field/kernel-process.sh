#!/bin/bash

# 切换到构建目录并清理
cd ~/data/OS-24Fall-FDU/build || exit
make clean > /dev/null

# 重复执行10次
for i in {1..10}; do
    # 清理构建目录
    # make clean

    # 在后台运行make qemu，并将输出重定向到proc.log
    (make qemu >proc.log) &

    # 等待0.5秒
    sleep 4

    # 检查proc.log中是否包含PASS
    if ! grep -q 'PASS' proc.log; then
        echo "> FAIL $i"
        exit 1
    else
        echo "> PASS $i"
    fi

    # 杀死后台运行的make qemu进程
    pkill -f 'make qemu'
done

exit 0
