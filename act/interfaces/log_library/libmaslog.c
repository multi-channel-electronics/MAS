#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include "mce/socks.h"
#include <libconfig.h>

#include "libmaslog.h"

#define CONFIG_FILE "/etc/mce/mas.cfg"

#define CONFIG_CLIENT "log_client"
#define CONFIG_LOGADDR "log_address"

#define CLIENT_NAME "\x1b" "LOG:client_name "


static
int destroy_exit(struct config_t *cfg, int error) {
	config_destroy(cfg);
	return error;
}

#define SUBNAME "logger_connect: "

static
int get_string(char *dest,
	       config_setting_t *parent, const char *name)
{
	config_setting_t *set =
		config_setting_get_member(parent, name);
	if (set==NULL) {
		fprintf(stderr, SUBNAME
			"key '%s' not found in config file\n",
			name);
		return -1;
	}
	strcpy(dest, config_setting_get_string(set));
	return 0;
}


int logger_connect(logger_t *logger, char *config_file, char *name)
{
	struct config_t cfg;
	config_init(&cfg);

	if (logger==NULL) {
		fprintf(stderr, SUBNAME "Null logger_t pointer!\n");
		return -1;
	}

	logger->fd = 0;

	if (config_file!=NULL) {
		if (!config_read_file(&cfg, config_file)) {
			fprintf(stderr,
				SUBNAME "Could not read config file '%s'\n",
				config_file);
			return -1;
		}
	} else {
		if (!config_read_file(&cfg, CONFIG_FILE)) {
			fprintf(stderr, SUBNAME
				"Could not read default configfile '%s'\n",
				CONFIG_FILE);
			return -1;
		}
	}

	config_setting_t *client = config_lookup(&cfg, CONFIG_CLIENT);
	
	char address[SOCKS_STR];
	if (get_string(address, client, CONFIG_LOGADDR)!=0)
		return destroy_exit(&cfg, -2);

	int sock = connect_to_addr(address);
	if (sock<0) {
		fprintf(stderr, SUBNAME "could not connect to logger at %s\n",
			address);
		return destroy_exit(&cfg, -3);
	}

	// Ignore pipe signals, we'll handle the errors (right?)
	signal(SIGPIPE, SIG_IGN);

	char cmd[SOCKS_STR];
	sprintf(cmd, "%c%s%s", '0'+LOGGER_ALWAYS, CLIENT_NAME, name);
	int sent = send(sock, cmd, strlen(cmd)+1, 0);
	if (sent != strlen(cmd)+1) {
		fprintf(stderr, SUBNAME "failed to send client name\n");
	}

	logger->fd = sock;

	return destroy_exit(&cfg, 0);
}

#undef SUBNAME

#define SUBNAME "logger_print: "

int logger_print(logger_t *logger, const char *str)
{
	return logger_print_level(logger, str, LOGGER_ALWAYS);
}

int logger_print_level(logger_t *logger, const char *str, int level)
{
	if (logger==NULL || logger->fd<=0) return -1;

	char packet[2048];
	int idx = 0;

	while (*str != 0) {
		packet[idx++] = '0' + level;
		while (*str != 0 && *str != '\n') {
			packet[idx++] = *str++;
		}
		if (*str == '\n') str++;
		packet[idx++] = 0;
	}

	int sent = send(logger->fd, packet, idx, 0);
	if (sent != idx) {
		if (sent==0) {
			fprintf(stderr, SUBNAME "connection closed, "
				"no further logging.\n");
			logger_close(logger);
		} else if (sent<0) {
			fprintf(stderr, SUBNAME "pipe error, errno=%i, "
				"no further logging.\n", errno);
			logger_close(logger);
		} else {
			fprintf(stderr, SUBNAME "partial send, "
				"logging will continue.\n");
		}
	}
	return (logger->fd > 0) ? 0 : -1;
}

#undef SUBNAME

#define SUBNAME "logger_write: "

int logger_write(logger_t *logger, const char *buf, int size)
{
	if (logger==NULL || logger->fd<=0) return -1;
	
	int sent = send(logger->fd, buf, size, 0);
	if (sent != size) {
		fprintf(stderr, SUBNAME "logging failed, (send error %i/%i)\n",
			sent, size);
		logger_close(logger);
		return -1;
	}
	return 0;
}

#undef SUBNAME


#define SUBNAME "logger_close: "

int logger_close(logger_t *logger)
{
	if (logger==NULL || logger->fd<=0) return -1;

	int fd = logger->fd;
	logger->fd = 0;

	int ret = close(fd);
	if (ret!=0) {
		fprintf(stderr, SUBNAME "close error, errno=%i\n", ret);
	}

	return ret;
}

#undef SUBNAME