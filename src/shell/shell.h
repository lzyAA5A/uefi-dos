/*
 * UEFI-DOS -- command interpreter (COMMAND.COM-like).
 */
#ifndef UEFIDOS_SHELL_H
#define UEFIDOS_SHELL_H

int  shell_init(void);
int  shell_run(void);                        /* console loop; returns exit code */
int  shell_exec_line(const char *line);      /* execute one command line */
int  shell_exec_batch_file(const char *path);/* execute a .BAT script */
void shell_set_echo(int on);

#endif
