#ifndef DISKIOSTRESS_SUBCOMMAND_H
#define DISKIOSTRESS_SUBCOMMAND_H

#include "types.h"

extern const CmdTbl_t cmdList[];

int command_parser(char* device, char* cmd_str, int cmd_argc, char* cmd_argv[]);

int single_read     (char* device, int argc, char* argv[]);
int single_write    (char* device, int argc, char* argv[]);
int single_trim     (char* device, int argc, char* argv[]);
int sequential_read (char* device, int argc, char* argv[]);
int sequential_write(char* device, int argc, char* argv[]);
int ramdom_read     (char* device, int argc, char* argv[]);
int ramdom_write    (char* device, int argc, char* argv[]);
int reset_controller(char* device, int argc, char* argv[]);
int fw_activation   (char* device, int argc, char* argv[]);
int log_page        (char* device, int argc, char* argv[]);

#endif /* DISKIOSTRESS_SUBCOMMAND_H */
