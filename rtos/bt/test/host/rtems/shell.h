/* host check stand-in for <rtems/shell.h> (rtos/bt/test/host_link.sh) */
#ifndef HOST_RTEMS_SHELL_H
#define HOST_RTEMS_SHELL_H
typedef int (*rtems_shell_command_t)(int argc, char **argv);
void *rtems_shell_add_cmd(const char *cmd, const char *topic, const char *usage, rtems_shell_command_t command);
#endif
