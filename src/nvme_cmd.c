#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include "nvme.h"
#include "nvme_cmd.h"

int nvme_read(int fd, char* buf, U64 lba, U32 len)
{
    int ret;
    struct nvme_user_io io;

    memset(&io, 0x00, sizeof(io));
    io.opcode  = nvme_cmd_read;
    io.slba    = lba;
    io.addr    = (unsigned long)buf;
    io.nblocks = len - 1;

    ret = ioctl(fd, NVME_IOCTL_SUBMIT_IO, (struct nvme_passthru_cmd*)&io);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_write(int fd, char* buf, U64 lba, U32 len, U32 write_hint)
{
    int ret;
    struct nvme_user_io io;

    memset(&io, 0x00, sizeof(io));
    io.opcode   = nvme_cmd_write;
    io.slba     = lba;
    io.addr     = (unsigned long)buf;
    io.nblocks  = len - 1;

    if (write_hint)
    {
        io.control |= NVME_RW_DTYPE_STREAMS;
        io.dsmgmt  |= (write_hint << 16);
    }

    ret = ioctl(fd, NVME_IOCTL_SUBMIT_IO, (struct nvme_passthru_cmd*)&io);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_flush(int fd, int nsid)
{
    int ret;
    struct nvme_passthru_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));
    cmd.opcode = nvme_cmd_flush;
    cmd.nsid   = nsid;

    ret = ioctl(fd, NVME_IOCTL_IO_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_trim(int fd, U64 lba, U32 len, U32 nsid)
{
    int ret;
    U32 nr_range = 1;
    struct nvme_dsm_range dsm_range;
    struct nvme_passthru_cmd cmd;

    dsm_range.slba = lba;
    dsm_range.nlb  = len;

    memset(&cmd, 0x0, sizeof(cmd));

    cmd.opcode   = nvme_cmd_dsm;
    cmd.nsid     = nsid;
    cmd.cdw10    = nr_range - 1;
    cmd.cdw11    = NVME_DSMGMT_AD;
    cmd.data_len = nr_range * sizeof(struct nvme_dsm_range);
    cmd.addr     = (unsigned long)&dsm_range;

    ret = ioctl(fd, NVME_IOCTL_IO_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_reset(int fd)
{
    return ioctl(fd, NVME_IOCTL_RESET);
}

int nvme_identify(int fd, int cns, int nsid, void* pBuff)
{
    int ret;
    struct nvme_passthru_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = nvme_admin_identify;
    cmd.nsid     = nsid;
    cmd.data_len = 4096;
    cmd.cdw10    = cns;
    cmd.addr     = (unsigned long)pBuff;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);

    if (ret)
    {
        return -1;
    }

    return 0;
}

int nvme_fw_download(int fd, char* pBuff, int data_len, int offset)
{
    int ret;
    struct nvme_passthru_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = nvme_admin_download_fw;
    cmd.data_len = data_len;
    cmd.cdw10    = (data_len >> 2) - 1,
    cmd.cdw11    = offset >> 2,
    cmd.addr     = (unsigned long)pBuff;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);

    if (ret)
    {
        return -1;
    }

    return 0;
}

int nvme_fw_commit(int fd, int action, int slot)
{
    int ret;
    struct nvme_passthru_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = nvme_admin_activate_fw;
    cmd.cdw10    = (action << 3) | slot,

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);

    if (ret)
    {
        return -1;
    }

    return 0;
}

int nvme_get_log(int fd, int logId, char* pBuff, int data_len)
{
    int ret;
    struct nvme_passthru_cmd cmd;
    U32 numd  = (data_len >> 2) - 1;
    U16 numdu = numd >> 16;
    U16 numdl = numd & 0xffff;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = nvme_admin_get_log_page;
    cmd.nsid     = 0xFFFFFFFF;
    cmd.data_len = data_len;
    cmd.cdw10    = (numdl << 16) | logId;
    cmd.cdw11    = numdu;
    cmd.addr     = (unsigned long)&pBuff[0];

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);

    if (ret)
    {
        return -1;
    }

    return 0;
}

int nvme_stream_get_status(int fd, int nsid, StreamStatus_t* status)
{
    int ret;
    struct nvme_directive_recv_cmd cmd;
    struct nvme_passthru_cmd* pcmd;

    pcmd = (struct nvme_passthru_cmd*)&cmd;
    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode     = nvme_admin_directive_recv;
    cmd.numd       = sizeof(StreamStatus_t) / sizeof(U32) - 1;
    cmd.nsid       = nsid;
    pcmd->addr     = (unsigned long)status;
    pcmd->data_len = sizeof(StreamStatus_t);
    cmd.doper      = NVME_DIR_RCV_ST_OP_STATUS;
    cmd.dtype      = NVME_DIR_STREAMS;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_stream_get_param(int fd, int nsid, struct streams_directive_params* params)
{
    int ret;
    struct nvme_directive_recv_cmd cmd;
    struct nvme_passthru_cmd* pcmd;

    pcmd = (struct nvme_passthru_cmd*)&cmd;
    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode     = nvme_admin_directive_recv;
    cmd.numd       = sizeof(struct streams_directive_params) / sizeof(U32) - 1;
    cmd.nsid       = nsid;
    pcmd->addr     = (unsigned long)params;
    pcmd->data_len = sizeof(struct streams_directive_params);
    cmd.doper      = NVME_DIR_RCV_ST_OP_PARAM;
    cmd.dtype      = NVME_DIR_STREAMS;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_stream_enable(int fd, int nsid, int enable)
{
    int ret;
    struct nvme_directive_send_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode = nvme_admin_directive_send;
    cmd.nsid   = nsid;
    cmd.doper  = NVME_DIR_SND_ID_OP_ENABLE;
    cmd.dtype  = NVME_DIR_IDENTIFY;
    cmd.tdtype = NVME_DIR_STREAMS;
    cmd.endir  = enable;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_stream_alloc_resource(int fd, int nsid, int num)
{
    int ret;
    struct nvme_directive_recv_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode = nvme_admin_directive_recv;
    cmd.numd   = 0;
    cmd.nsid   = nsid;
    cmd.doper  = NVME_DIR_RCV_ST_OP_RESOURCE;
    cmd.dtype  = NVME_DIR_STREAMS;
    cmd.nsr    = num;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_stream_rel_resource(int fd, int nsid)
{
    int ret;
    struct nvme_directive_send_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode = nvme_admin_directive_send;
    cmd.nsid   = nsid;
    cmd.doper  = NVME_DIR_SND_ST_OP_REL_RSC;
    cmd.dtype  = NVME_DIR_STREAMS;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}

int nvme_stream_rel_id(int fd, int nsid, int id)
{
    int ret;
    struct nvme_directive_send_cmd cmd;

    memset(&cmd, 0x00, sizeof(cmd));

    cmd.opcode = nvme_admin_directive_send;
    cmd.nsid   = nsid;
    cmd.doper  = NVME_DIR_SND_ST_OP_REL_ID;
    cmd.dtype  = NVME_DIR_STREAMS;
    cmd.dspec  = id;

    ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, (struct nvme_passthru_cmd*)&cmd);

    if (ret != 0)
    {
        return -1;
    }

    return 0;
}
