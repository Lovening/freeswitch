include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_rtpforward

# libwebsockets 路径
LWS_DIR = $(abs_srcdir)/libwebsockets
LWS_BUILD = $(LWS_DIR)/build
LWS_LIB = $(LWS_BUILD)/lib/libwebsockets.a

# 编译 libwebsockets（如果不存在）
$(LWS_LIB):
	@echo "Building libwebsockets..."
	@cd $(LWS_DIR) && mkdir -p build && cd build && \
	cmake .. \
		-DLWS_WITH_SSL=OFF \
		-DLWS_WITHOUT_SERVER=ON \
		-DLWS_WITHOUT_TESTAPPS=ON \
		-DLWS_WITH_STATIC=ON \
		-DLWS_WITH_SHARED=OFF \
		-DLWS_WITH_HTTP2=OFF \
		-DLWS_IPV6=OFF \
		-DDISABLE_WERROR=ON \
		-DCMAKE_C_FLAGS="-fPIC -Wno-error" \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DCMAKE_BUILD_TYPE=Release && \
	$(MAKE) -j4
	@echo "libwebsockets built successfully"

mod_LTLIBRARIES = mod_rtpforward.la
mod_rtpforward_la_SOURCES  = mod_rtpforward.c ws_client.c ws_client.h
mod_rtpforward_la_CFLAGS   = $(AM_CFLAGS) -I$(LWS_DIR)/include -I$(LWS_BUILD)/include
mod_rtpforward_la_LIBADD   = $(switch_builddir)/libfreeswitch.la $(LWS_LIB)
mod_rtpforward_la_LDFLAGS  = -avoid-version -module -no-undefined -shared
mod_rtpforward_la_DEPENDENCIES = $(LWS_LIB)

# 确保所有 .c 文件编译前先构建 libwebsockets
$(mod_rtpforward_la_OBJECTS): $(LWS_LIB)

clean-local:
	@echo "Cleaning libwebsockets build..."
	@rm -rf $(LWS_BUILD)

.PHONY: clean-local