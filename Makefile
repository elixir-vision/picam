
# Path to the VideoCore headers and libraries
# Override if this is different on your system
VC_DIR?=/opt/vc

SRCS=src/raspijpgs.c
INCLUDES?=-I$(VC_DIR)/include -I$(VC_DIR)/include/interface/vcos/pthreads -I$(VC_DIR)/include/interface/vmcs_host/linux
LIBS=-L$(VC_DIR)/lib -lmmal_core -lmmal_util -lmmal_vc_client -Lvcos -lbcm_host -lm
OBJS=$(SRCS:.c=.o)
CFLAGS?=-Wall -O2
LDFLAGS?=
STRIP?=strip

all: priv/raspijpgs
priv/raspijpgs: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(OBJS): %.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# This build target is just for travis-ci so that we can check for warnings and
# compilation errors automatically. In the coverity branch, this will also run
# static analysis.
travis:
	if [ ! -e userland ]; then \
	    git clone --depth=1 https://github.com/raspberrypi/userland.git; \
	fi
	INCLUDES="-Iuserland/host_applications/linux/libs/bcm_host/include -Iuserland -Iuserland/interface/vcos/pthreads -Iuserland/interface/vmcs_host/linux" $(MAKE) priv/raspijpgs.o

clean:
	rm -f $(OBJS) priv/raspijpgs

.PHONY: all raspijpgs clean
