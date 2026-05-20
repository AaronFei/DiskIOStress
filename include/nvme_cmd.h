#ifndef DISKIOSTRESS_NVME_CMD_H
#define DISKIOSTRESS_NVME_CMD_H

#include "types.h"

int nvme_read (int fd, char* buf, U64 lba, U32 len);
int nvme_write(int fd, char* buf, U64 lba, U32 len, U32 write_hint);
int nvme_flush(int fd, int nsid);
int nvme_trim (int fd, U64 lba, U32 len, U32 nsid);
int nvme_reset(int fd);
int nvme_identify(int fd, int cns, int nsid, void* pBuff);
int nvme_fw_download(int fd, char* pBuff, int data_len, int offset);
int nvme_fw_commit(int fd, int action, int slot);
int nvme_get_log(int fd, int logId, char* pBuff, int data_len);

int nvme_stream_get_status(int fd, int nsid, StreamStatus_t* status);
int nvme_stream_get_param (int fd, int nsid, struct streams_directive_params* params);
int nvme_stream_enable    (int fd, int nsid, int enable);
int nvme_stream_alloc_resource(int fd, int nsid, int num);
int nvme_stream_rel_resource  (int fd, int nsid);
int nvme_stream_rel_id        (int fd, int nsid, int id);

#endif /* DISKIOSTRESS_NVME_CMD_H */
