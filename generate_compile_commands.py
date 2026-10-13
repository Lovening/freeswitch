#!/usr/bin/env python3
"""
为 FreeSWITCH 项目生成 compile_commands.json
用于 clangd 等语言服务器识别头文件和编译选项
"""

import json
import os
import glob

# FreeSWITCH 项目根目录
FREESWITCH_ROOT = "/home/ning/workSpace/freeswitch"

# 基础编译标志
BASE_FLAGS = [
    "-std=c99",
    "-D_GNU_SOURCE",
    "-DHAVE_CONFIG_H",
    "-DSWITCH_HAVE_YUV",
    "-DSWITCH_HAVE_VPX",
    f"-I{FREESWITCH_ROOT}/src/include",
    f"-I{FREESWITCH_ROOT}/libs/apr/include",
    f"-I{FREESWITCH_ROOT}/libs/apr-util/include",
    f"-I{FREESWITCH_ROOT}/libs/sqlite",
    f"-I{FREESWITCH_ROOT}/libs/pcre",
    f"-I{FREESWITCH_ROOT}/libs/libtpl-1.5/src",
    f"-I{FREESWITCH_ROOT}/libs/srtp/include",
    f"-I{FREESWITCH_ROOT}/libs/srtp/crypto/include",
    "-I/usr/include",
    "-I/usr/local/include",
    "-fPIC",
    "-Wno-unused-parameter",
]

def find_source_files(directory):
    """递归查找所有 C 源文件"""
    c_files = []
    for root, dirs, files in os.walk(directory):
        # 跳过一些不需要的目录
        skip_dirs = [
            '.git', '.libs', '.deps', 'build', 'test-apps', 'test',
            # 第三方库目录
            'libwebsockets', 'libapr', 'libsqlite', 'libtool',
            # 文档和资源
            'doc-assets', 'docs', 'assets', 'images'
        ]
        # 跳过已经单独列出的模块目录（避免重复）
        if directory == FREESWITCH_ROOT + "/src":
            skip_dirs.extend(['mod', 'tests'])
        dirs[:] = [d for d in dirs if d not in skip_dirs]
        for file in files:
            if file.endswith('.c'):
                c_files.append(os.path.join(root, file))
    return c_files

def generate_compile_command(source_file):
    """为单个源文件生成编译命令"""
    flags = BASE_FLAGS.copy()
    
    # 为特定模块添加特殊包含路径
    if "mod_rtpforward" in source_file:
        mod_dir = os.path.dirname(source_file)
        flags.extend([
            f"-I{mod_dir}/libwebsockets/include",
            f"-I{mod_dir}/libwebsockets/build/include",
        ])
    
    return {
        "directory": FREESWITCH_ROOT,
        "command": f"gcc {' '.join(flags)} -c {source_file}",
        "file": source_file
    }

def main():
    compile_commands = []
    
    # 查找所有源文件
    print("正在扫描源文件...")
    directories = [
        f"{FREESWITCH_ROOT}/src",
        f"{FREESWITCH_ROOT}/src/mod/applications",
        f"{FREESWITCH_ROOT}/src/mod/asr_tts",
        f"{FREESWITCH_ROOT}/src/mod/codecs",
        f"{FREESWITCH_ROOT}/src/mod/databases",
        f"{FREESWITCH_ROOT}/src/mod/directories",
        f"{FREESWITCH_ROOT}/src/mod/dialplans",
        f"{FREESWITCH_ROOT}/src/mod/endpoints",
        f"{FREESWITCH_ROOT}/src/mod/event_handlers",
        f"{FREESWITCH_ROOT}/src/mod/formats",
        f"{FREESWITCH_ROOT}/src/mod/languages",
        f"{FREESWITCH_ROOT}/src/mod/loggers",
        f"{FREESWITCH_ROOT}/src/mod/say",
        f"{FREESWITCH_ROOT}/src/mod/timer",
        f"{FREESWITCH_ROOT}/src/mod/xml_int",
    ]
    
    for directory in directories:
        if os.path.exists(directory):
            source_files = find_source_files(directory)
            print(f"在 {directory} 中找到 {len(source_files)} 个源文件")
            
            for source_file in source_files:
                compile_commands.append(generate_compile_command(source_file))
    
    # 写入 compile_commands.json
    output_file = f"{FREESWITCH_ROOT}/compile_commands.json"
    with open(output_file, 'w') as f:
        json.dump(compile_commands, f, indent=2)
    
    print(f"已生成 {output_file}，包含 {len(compile_commands)} 个编译命令")

if __name__ == "__main__":
    main()
