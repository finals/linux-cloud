# linux
用于保存linux kernel的修改模块，根据kernel的LTS分支持续修改有必要的部分。

# 用法
下载Linux kernel源码，解压

将修改模块应用于kernel源码
./apply.sh bcache [linux kernel 源码的路径]

输入 yes，以覆盖原有代码

应用完后，正常编译kernel即可

