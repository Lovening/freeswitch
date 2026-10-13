#!/bin/bash
# 检查 HAVE_LIBAVFILTER 宏是否被定义

echo "========================================"
echo "检查 FFmpeg libavfilter 编译状态"
echo "========================================"
echo

# 1. 检查 configure 状态
echo "1. 检查 config.status 中的 libavfilter 配置:"
if [ -f config.status ]; then
    grep -i "libavfilter\|HAVE_LIBAVFILTER" config.status | head -5
else
    echo "   config.status 不存在，需要运行 ./configure"
fi
echo

# 2. 检查已编译的模块
echo "2. 检查已编译模块是否链接了 libavfilter:"
if [ -f src/mod/applications/mod_conference/.libs/mod_conference.so ]; then
    ldd src/mod/applications/mod_conference/.libs/mod_conference.so | grep avfilter
    if [ $? -eq 0 ]; then
        echo "   ✓ 模块已链接 libavfilter"
    else
        echo "   ✗ 模块未链接 libavfilter"
    fi
else
    echo "   mod_conference.so 不存在"
fi
echo

# 3. 检查编译时的宏定义
echo "3. 检查编译时使用的 CFLAGS:"
if [ -f src/mod/applications/mod_conference/Makefile ]; then
    grep "LIBAVFILTER" src/mod/applications/mod_conference/Makefile | head -3
else
    echo "   Makefile 不存在"
fi
echo

# 4. 检查 pkg-config
echo "4. 检查系统是否安装了 libavfilter:"
pkg-config --modversion libavfilter 2>/dev/null
if [ $? -eq 0 ]; then
    echo "   ✓ libavfilter 已安装"
    echo "   CFLAGS: $(pkg-config --cflags libavfilter)"
    echo "   LIBS: $(pkg-config --libs libavfilter)"
else
    echo "   ✗ libavfilter 未安装或 pkg-config 未找到"
fi
echo

# 5. 提供修复建议
echo "========================================"
echo "修复步骤（如果 filter 未启用）:"
echo "========================================"
echo "1. 安装 FFmpeg 开发库:"
echo "   sudo apt-get install libavfilter-dev libavutil-dev"
echo
echo "2. 重新配置和编译:"
echo "   ./bootstrap.sh -j"
echo "   ./configure --enable-libavfilter"
echo "   make mod_conference-clean"
echo "   make mod_conference-install"
echo
echo "3. 重启 FreeSWITCH:"
echo "   fs_cli -x 'reload mod_conference'"
echo
echo "4. 查看日志确认:"
echo "   应该看到: 'FFmpeg libavfilter support is ENABLED'"
echo "========================================"
