# FFmpeg libavfilter 支持默认启用

## 变更说明

从当前版本开始，FreeSWITCH 的 FFmpeg libavfilter 支持已**默认启用**。

### 主要变更

1. **自动检测**：configure 脚本会自动检测系统中的 libavfilter 和 libavutil 库
2. **默认启用**：如果检测到 FFmpeg 库，将自动启用 filter 支持
3. **无需额外配置**：不再需要手动指定 `--enable-libavfilter`

### 影响的模块

- **mod_conference**：支持在视频流上添加用户名称/ID叠加层

### 依赖安装

在编译之前，请确保安装了 FFmpeg 开发库：

```bash
# Debian/Ubuntu
sudo apt-get install libavfilter-dev libavutil-dev

# CentOS/RHEL
sudo yum install ffmpeg-devel

# macOS
brew install ffmpeg
```

### 编译步骤

```bash
# 1. 重新生成配置脚本（首次或修改 configure.ac 后）
./bootstrap.sh -j

# 2. 运行配置（libavfilter 将自动启用）
./configure

# 3. 编译和安装
make
sudo make install
```

### 禁用 FFmpeg 支持

如果不需要 FFmpeg filter 支持，可以显式禁用：

```bash
./configure --disable-libavfilter
```

### 检查编译状态

在 configure 运行后，检查输出以确认 libavfilter 状态：

```bash
./configure | grep -i avfilter
```

成功启用时，您应该看到类似输出：
```
checking for LIBAVFILTER... yes
```

如果未检测到库，将显示警告：
```
configure: WARNING: libavfilter not found, FFmpeg filter support will be disabled
```

### 配置文件变更

#### configure.ac

新增了 libavfilter 的自动检测配置：

```autoconf
AC_ARG_ENABLE(libavfilter,
[AC_HELP_STRING([--disable-libavfilter],[build without FFmpeg libavfilter support])],
[enable_libavfilter="$enableval"],[enable_libavfilter="yes"])

if test "${enable_libavfilter}" = "yes" ; then
    PKG_CHECK_MODULES([LIBAVFILTER], [libavfilter libavutil], 
                      [have_libavfilter="yes"], [have_libavfilter="no"])
    if test "${have_libavfilter}" = "yes" ; then
        AC_DEFINE([HAVE_LIBAVFILTER], [1], [Define if you have libavfilter])
    fi
fi

AM_CONDITIONAL([HAVE_LIBAVFILTER],[test "${enable_libavfilter}" = "yes"])
```

#### src/mod/applications/mod_conference/Makefile.am

新增了条件编译支持：

```makefile
if HAVE_LIBAVFILTER
mod_conference_la_CFLAGS += $(LIBAVFILTER_CFLAGS)
mod_conference_la_LIBADD += $(LIBAVFILTER_LIBS)
endif
```

### 升级指南

#### 从旧版本升级

如果您之前使用 `--enable-libavfilter` 选项编译：

1. 该选项仍然有效（向后兼容）
2. 但现在可以省略该选项，效果相同
3. 建议使用标准配置流程：`./configure`

#### 首次使用

按照上述"编译步骤"操作即可。

### 故障排除

#### 问题：configure 提示找不到 libavfilter

**原因**：系统未安装 FFmpeg 开发库

**解决**：
```bash
# 检查是否安装
pkg-config --modversion libavfilter

# 如未安装，根据系统安装对应包
sudo apt-get install libavfilter-dev libavutil-dev  # Debian/Ubuntu
```

#### 问题：编译时出现 FFmpeg 相关错误

**解决**：
1. 确认 FFmpeg 版本 >= 3.0
   ```bash
   ffmpeg -version
   ```

2. 如果版本过低或不兼容，可以禁用：
   ```bash
   ./configure --disable-libavfilter
   ```

#### 问题：运行时 filter 不生效

**检查**：
1. 确认模块正确编译：
   ```bash
   ldd /usr/local/freeswitch/mod/mod_conference.so | grep avfilter
   ```

2. 查看 FreeSWITCH 日志，检查是否有 filter 初始化消息

### 更多信息

详细的 FFmpeg filter 使用说明，请参考：
- `src/mod/applications/mod_conference/FFMPEG_FILTER_README.md`

### 技术支持

如有问题，请提交 Issue 或查看 FreeSWITCH 官方文档。
