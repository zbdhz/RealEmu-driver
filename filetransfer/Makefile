# 变量
LOCAL_DIR_TO = /home/emu/dev/RealEmu-driver
REMOTE_DIR_TO = /home/gtx/dev

LOCAL_DIR_FROM = /home/emu/dev/RealEmu-driver
REMOTE_DIR_FROM = /home/gtx/dev/XDMA

REMOTE_HOST = gtx@114.212.115.105
# REMOTE_HOST = wjz@114.212.118.252

# 默认目标
all: copy_to_remote

# 拷贝目录到远程
copy_to_remote:
	scp -r $(LOCAL_DIR_TO) $(REMOTE_HOST):$(REMOTE_DIR_TO)

copy_from_remote:
	scp -r $(REMOTE_HOST):$(REMOTE_DIR_FROM) $(LOCAL_DIR_FROM)

from: copy_from_remote

to: copy_to_remote

# 其他目标...