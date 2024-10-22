################################################################################
# Copyright (c) 2020-2023, NVIDIA CORPORATION. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
################################################################################


TARGET_DEVICE = $(shell gcc -dumpmachine | cut -f1 -d -)

# For cloud
CUDA_VER=12.1
NVDS_VERSION:=6.3

# Overwriting CUDA version if Jetson board detected
ifeq ($(TARGET_DEVICE),aarch64)
  CUDA_VER=11.4
endif

ifeq ($(CUDA_VER),)
  $(error "CUDA_VER is not set")
endif

APP:= inference_pipeline

LIB_INSTALL_DIR?=/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/lib/
APP_INSTALL_DIR?=./

ifeq ($(TARGET_DEVICE),aarch64)
  CFLAGS:= -DPLATFORM_TEGRA
endif

SRCS:= $(wildcard src/*.cpp)

INCS:= $(wildcard include/*.h) 

PKGS:= gstreamer-1.0

OBJS:= $(SRCS:.cpp=.o)

CFLAGS+= -I/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/sources/includes \
		 -I /usr/local/cuda-$(CUDA_VER)/include \
		 -I include/ -I/usr/local/include

CFLAGS+= $(shell pkg-config --cflags $(PKGS))

LIBS:= $(shell pkg-config --libs $(PKGS))

LIBS+= -L$(LIB_INSTALL_DIR) -lnvdsgst_meta -lnvds_meta -lnvdsgst_helper -lm -lnvdsgst_smartrecord\
       	-L/usr/local/cuda-$(CUDA_VER)/lib64/ -lcudart \
	   -Wl,-rpath,$(LIB_INSTALL_DIR) -lglog \
	   -lSimpleAmqpClient -lrabbitmq -lpthread \
	   -L/usr/local/cuda-$(CUDA_VER)/lib64/stubs/ -lcuda \


all: $(APP)
	@echo "Building for TARGET_DEVICE: $(TARGET_DEVICE)"

%.o: %.cpp $(INCS) Makefile
	$(CXX) -c -o $@ $(CFLAGS) $<

$(APP): $(OBJS) Makefile
	$(CXX) -o $(APP) $(OBJS) $(LIBS)

install: $(APP)
	cp -rv $(APP) $(APP_INSTALL_DIR)

clean:
	rm -rf $(OBJS) $(APP)
