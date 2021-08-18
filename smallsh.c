#define _POSIX_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/signal.h>

#define MAX_INPUT_SIZE 2048

struct user_input_t
{
	char **argv;
	char *input;
	char *output;
	int is_bg;
};

struct child_processes_t
{
	pid_t process[512];
	int size;
};

int g_allow_background = 1;
struct sigaction SIGINT_action = {0};
struct sigaction SIGTSTP_action = {0};

struct child_processes_t g_child_processes;

void err_sys(char *message);

void *secure_malloc(int size);
char *secure_strdup(char *source);

int count_words(char *line);
int is_comment(char *line);

char **build_items(char *line);
char **get_argv(char **items);
char *get_input(char **items);
char *get_output(char **items);
int is_background(char **items);
void insert_child_process(pid_t pid);
void remove_child_process(pid_t pid);
void check_child_processes();
void kill_all_child_processes();

struct user_input_t *parse_user_input(char *line);
void print_user_input(struct user_input_t *usrinp);
char *expand_dollar_symbol(char *s);
void redirect_input(char *file_name);
void redirect_output(char *file_name);
int exec_cmd(struct user_input_t *cmd);
void change_dir(struct user_input_t *cmd);
void print_status(int code);
void handle_SIGTSTP(int signo);

int main(void)
{
	char command[MAX_INPUT_SIZE];
	char *command_expanded;
	struct user_input_t *cmd;
	int status_code = 0;
	g_child_processes.size = 0;
	g_allow_background = 1;

	SIGINT_action.sa_handler = SIG_IGN;
	sigfillset(&SIGINT_action.sa_mask);
	SIGINT_action.sa_flags = 0;
	sigaction(SIGINT, &SIGINT_action, NULL);

	SIGTSTP_action.sa_handler = handle_SIGTSTP;
	sigfillset(&SIGTSTP_action.sa_mask);
	SIGTSTP_action.sa_flags = SA_NOCLDSTOP;
	sigaction(SIGTSTP, &SIGTSTP_action, NULL);

	while (1)
	{
		printf(": ");
		fflush(NULL);
		fgets(command, MAX_INPUT_SIZE, stdin);
		command[strlen(command) - 1] = '\0'; //gỡ bỏ kí tự '\n'

		command_expanded = expand_dollar_symbol(command);
		cmd = parse_user_input(command_expanded);
		if (!cmd || cmd->argv[0][0] == '\0')
		{
			continue;
		}
		//print_user_input(cmd);
		if (!strcmp(cmd->argv[0], "exit"))
		{
			kill_all_child_processes();
			break;
		}
		if (!strcmp(cmd->argv[0], "cd"))
		{
			change_dir(cmd);
			continue;
		}
		if (!strcmp(cmd->argv[0], "status"))
		{
			print_status(status_code);
			continue;
		}
		else
		{
			status_code = exec_cmd(cmd);
		}
		check_child_processes();
		free(cmd);
	}
	return EXIT_SUCCESS;
}

/*
* Thông báo lỗi hệ thống và thoát chương trình
*/
void err_sys(char *message)
{
	perror(message);
	exit(EXIT_FAILURE);
}

/*
* Chuyển hướng luồng dữ liệu vào
*/
void redirect_input(char *file_name)
{
	int input_fd = open(file_name, O_RDONLY);
	if (input_fd == -1)
	{
		fprintf(stdout, "cannot open %s for input\n", file_name);
		fflush(stdout);
		exit(EXIT_FAILURE);
	}

	fcntl(input_fd, F_SETFD, FD_CLOEXEC);
	if (dup2(input_fd, STDIN_FILENO) == -1)
	{
		err_sys("dup2");
	}
}

/*
* Chuyển hướng luồng dữ liệu ra
*/
void redirect_output(char *file_name)
{
	int output_fd = open(file_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (output_fd == -1)
	{
		fprintf(stdout, "cannot open %s for output\n", file_name);
		fflush(stdout);
		exit(EXIT_FAILURE);
	}

	fcntl(output_fd, F_SETFD, FD_CLOEXEC);
	if (dup2(output_fd, STDOUT_FILENO) == -1)
	{
		err_sys("dup2");
	}
}

/*
* Thực hiện các lệnh hệ thống (các lệnh KHÔNG phải cd, status, exit, #comment)
*/
int exec_cmd(struct user_input_t *cmd)
{
	int status_code;
	pid_t child_pid = fork();
	if (child_pid == -1)
	{
		err_sys("fork");
	}
	else if (child_pid == 0) //---- mục này là phần của child process
	{
		if (cmd->input) //nếu có nhập "< input" thực hiện chuyển luồng dữ liệu vào
		{
			redirect_input(cmd->input);
			if (cmd->is_bg) //nếu là process nền thì chuyển hướng vào /dev/null
			{
				redirect_input("/dev/null");
			}
		}
		if (cmd->output) //nếu có nhập "> output" thực hiện chuyển luồng dữ liệu ra
		{
			redirect_output(cmd->output);
			if (cmd->is_bg) //nếu là process nền thì chuyển hướng vào /dev/null
			{
				redirect_output("/dev/null");
			}
		}
		if (cmd->is_bg == 0)
		{
			//Khôi phục lại signal cho các process nổi (KHÔNG phải process nền thì khôi phục signal)
			SIGINT_action.sa_handler = SIG_DFL;
			SIGINT_action.sa_flags = 0;
			sigaction(SIGINT, &SIGINT_action, NULL);
		}

		//Thực hiện lệnh hệ thống bằng hàm execvp, có kiểm tra lỗi
		if (execvp(cmd->argv[0], cmd->argv) == -1)
		{
			fprintf(stdout, "%s: no such file or directory\n", cmd->argv[0]);
			fflush(stdout);
			exit(EXIT_FAILURE);
		}
	}
	else //---- còn đây là phần của parent process
	{
		if (cmd->is_bg) //nếu là process nền thì không cần đợi nó xong (sử dụng WNOHANG)
		{
			waitpid(child_pid, &status_code, WNOHANG);
			printf("Background pid is %d\n", child_pid);
			fflush(stdout);
			insert_child_process(child_pid);
		}
		else //nếu là process nổi thì phải đợi nó xong (sử dụng 0)
		{
			waitpid(child_pid, &status_code, 0);
			//Nếu trong quá trình đợi process nổi xong, có signal ngắt được nhập vào thì thông báo
			if (WIFSIGNALED(status_code) != 0)
			{
				printf("terminated by signal %d\n", WTERMSIG(status_code));
				fflush(stdout);
			}
		}
	}
	return status_code;
}

/*
* hàm thực hiện chuyển thư mục hiện hành
*/
void change_dir(struct user_input_t *cmd)
{
	//Nếu không có đối số đường dẫn cụ thể, hoặc đường dẫn là "~" thì sẽ tự chuyển đến HOME
	if (cmd->argv[1] == NULL || !strcmp(cmd->argv[1], "~"))
	{
		char *home_dir = getenv("HOME");
		chdir(home_dir);
	}
	else //còn nếu có đối số đường dẫn cụ thể, thì chuyển tới đó
	{
		chdir(cmd->argv[1]);
	}
}

/*
* Hàm cấp phát bộ nhớ an toàn, có thông báo khi không đủ bộ nhớ
*/
void *secure_malloc(int size)
{
	void *res = malloc(size);
	if (!res)
	{
		err_sys("malloc");
	}
	return res;
}

/*
* Hàm sao chép chuỗi an toàn, có thông báo khi không đủ bộ nhớ
*/
char *secure_strdup(char *source)
{
	char *res = (char *)secure_malloc(strlen(source) + 1);
	strcpy(res, source);
	return res;
}

/*
* Hàm đếm số lượng items đã nhập vào, giúp cấp phát bộ nhớ dủ số lượng
* ví dụ: wc < junk > junk2
* hàm sẽ trả về 5, vì có 5 items là: wc, <, junk, >, junk2
*/
int count_words(char *line)
{
	int words = 0, in_word = 0;
	while (*line)
	{
		if (isspace(*line))
		{
			in_word = 0;
		}
		else
		{
			if (in_word == 0)
			{
				words++;
				in_word = 1;
			}
		}
		line++;
	}
	return words;
}

/*
* Hàm kiểm tra một lệnh nhập vào có phải là comment hay không?
* trả về 1 nếu đúng, 0 nếu sai
*/
int is_comment(char *line)
{
	int i = 0;
	while (isspace(line[i]))
	{
		i++;
	}
	if (line[i] == '#')
	{
		return 1;
	}
	return 0;
}

/*
* Hàm tách riêng lẻ từng items đã nhập.
Vì các items viết liền nhau trên 1 dòng cần tách riêng để dễ xử lý
*/
char **build_items(char *line)
{
	int i = 0, argc = 0;
	char *next = NULL;
	char **argv = NULL;
	if (line == NULL || is_comment(line))
	{
		return NULL;
	}

	argc = count_words(line);
	if (argc == 0)
	{
		return NULL;
	}
	argv = (char **)secure_malloc(sizeof(char *) * (argc + 1));

	for (i = 0; i < argc; ++i)
	{
		while (isspace(*line))
		{
			line++;
		}

		next = line;
		while (*next && !isspace(*next))
		{
			next++;
		}
		*next = '\0';

		argv[i] = secure_strdup(line);
		line = next + 1;
	}

	argv[i] = NULL;
	return argv;
}

/*
* Hàm chỉ lấy các tham số từ các items, không lấy tùy chọn điều hướng input và output
* ví du: ls -l > junk
* thì hàm này sẽ lấy ra: ls -l
*/
char **get_argv(char **items)
{
	char **argv = NULL;
	int i = 0, size = 0;
	while (items[i] != NULL)
	{
		if (!strcmp(items[i], "<"))
			break;

		if (!strcmp(items[i], ">"))
			break;

		if (!strcmp(items[i], "&"))
			break;

		i++;
		size++;
	}

	argv = (char **)secure_malloc(sizeof(char *) * (size + 1));
	i = 0;
	while (i < size)
	{
		argv[i] = secure_strdup(items[i]);
		i++;
	}
	argv[size] = NULL;
	return argv;
}

/*
* Hàm lấy file input điều hướng đã nhập, nếu không có trả về NULL
*/
char *get_input(char **items)
{
	char *input = NULL;
	int i = 0;
	while (items[i])
	{
		if (!strcmp(items[i], "<") && *items[i + 1])
		{
			input = secure_strdup(items[i + 1]);
			break;
		}
		i++;
	}
	return input;
}

/*
* Hàm lấy file output điều hướng đã nhập, nếu không có trả về NULL
*/
char *get_output(char **items)
{
	char *output = NULL;
	int i = 0;
	while (items[i])
	{
		if (!strcmp(items[i], ">") && *items[i + 1])
		{
			output = secure_strdup(items[i + 1]);
			break;
		}
		i++;
	}
	return output;
}

/*
* Hàm kiểm tra xem một lệnh có phải là lệnh chạy nền hay không (có dấu & ở cuối)
* trả về 1 nếu đúng, 0 nếu sai
*/
int is_background(char **items)
{
	int background = 0, i = 0;
	while (items[i])
	{
		if (!strcmp(items[i], "&"))
		{
			background = 1;
			break;
		}
		i++;
	}
	return background;
}

/*
* Hàm gộp, tách dữ liệu nhập theo dòng (line) thành cấu trúc user_input_t
* Đây là cấu trúc các mục khi đã tách và phân tích
*/
struct user_input_t *parse_user_input(char *line)
{
	struct user_input_t *usrinp;
	char **items = build_items(line);
	if (items == NULL)
	{
		return NULL;
	}

	usrinp = (struct user_input_t *)secure_malloc(sizeof(struct user_input_t));
	usrinp->argv = get_argv(items);
	usrinp->input = get_input(items);
	usrinp->output = get_output(items);
	if (g_allow_background)
	{
		usrinp->is_bg = is_background(items);
	}
	else
	{
		usrinp->is_bg = 0;
	}
	return usrinp;
}

/*
* Hàm in ra các thành phần của print_user_input để kiểm tra 
*/
void print_user_input(struct user_input_t *usrinp)
{
	int i;
	printf("Command: [%s]\n", usrinp->argv[0]);
	printf("Arguments: ");
	for (i = 0; usrinp->argv[i]; i++)
	{
		printf("%s ", usrinp->argv[i]);
	}
	printf("\nInput: %s\n", usrinp->input);
	printf("Output: %s\n", usrinp->output);
	printf("Background: %d\n", usrinp->is_bg);
	printf("=======\n");
	fflush(stdout);
}

/*
* Hàm in trạng thái kết thúc của lệnh trước đó
*/
void print_status(int code)
{
	if (WIFEXITED(code) != 0)
	{
		fprintf(stdout, "exit value %d\n", WEXITSTATUS(code));
		fflush(stdout);
	}
	else if (WIFSIGNALED(code) != 0)
	{
		fprintf(stdout, "terminated by signal %d\n", WTERMSIG(code));
		fflush(stdout);
	}
}

/*
* Chèn thêm một tiến trình con vào danh sách nhớ
* Điều này giúp cho việc thoát chương trình sẽ KILL chúng, nếu chúng còn sống
*/
void insert_child_process(pid_t pid)
{
	int next_pos = g_child_processes.size;
	g_child_processes.process[next_pos] = pid;
	g_child_processes.size++;
}

/*
* Gỡ bỏ một tiến trình con vào danh sách nhớ khi chúng đã kết thúc
*/
void remove_child_process(pid_t pid)
{
	int i, last_pos = g_child_processes.size - 1;
	for (i = 0; i < g_child_processes.size; ++i)
	{
		if (g_child_processes.process[i] == pid)
		{
			g_child_processes.process[i] = g_child_processes.process[last_pos];
			g_child_processes.size--;
		}
	}
}

/*
* Hàm kiểm tra trong danh sách nhớ thì tiến trình nào đã kết thúc,
* hiện thông báo pid của tiến trình đó.
*/
void check_child_processes()
{
	pid_t child_pid;
	int code;
	while ((child_pid = waitpid(-1, &code, WNOHANG)) > 0)
	{
		printf("background pid %d is done: ", child_pid);
		fflush(stdout);
		print_status(code);
	}
}

/*
* Hàm KILL tất cả các tiến trình con, điều này giúp làm sạch tài nguyên
* khi kết thúc (lệnh exit)
*/
void kill_all_child_processes()
{
	int i;
	pid_t child_pid;
	for (i = 0; i < g_child_processes.size; ++i)
	{
		child_pid = g_child_processes.process[i];
		kill(child_pid, SIGTERM);
	}
}

/*
* Hàm xử lý tín hiệu Ctrl+Z
*/
void handle_SIGTSTP(int signo)
{
	if (g_allow_background)
	{
		fprintf(stdout, "\nEntering foreground-only mode (& is now ignored)\n");
		g_allow_background = 0;
	}
	else
	{
		fprintf(stdout, "\nExiting foreground-only mode\n");
		g_allow_background = 1;
	}
	fprintf(stdout, ": ");
	fflush(stdout);
}

/*
* Hàm thay thế cặp kí hiệu $$ bằng pid 
*/
char *expand_dollar_symbol(char *s)
{
	char *result;
	char pid_str[8] = {0};
	int i, pid_length, cnt = 0;

	pid_t pid = getpid();
	sprintf(pid_str, "%d", pid);
	pid_length = strlen(pid_str);

	for (i = 0; s[i] != '\0'; i++)
	{
		if (strstr(&s[i], "$$") == &s[i])
		{
			cnt++;
			i += pid_length - 1;
		}
	}

	result = (char *)malloc(i + cnt * (pid_length - 2) + 1);

	i = 0;
	while (*s)
	{
		if (strstr(s, "$$") == s)
		{
			strcpy(&result[i], pid_str);
			i += pid_length;
			s += 2;
		}
		else
		{
			result[i++] = *s++;
		}
	}

	result[i] = '\0';
	return result;
}