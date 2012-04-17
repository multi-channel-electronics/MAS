#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <mce_library.h>

#include <sys/ioctl.h>
#include <errno.h>
#include <mce/data_ioctl.h>


int main() {
	char dev_name[] = "/dev/mce_data0";
	int fd = open(dev_name, O_RDWR);

	// You can run non-blocking if you want -- but if acqs happen
	// in quick succession you are more likely to miss them.
/* 	if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)) { */
/* 		printf("set nonblock failed\n"); */
/* 	} */
	FILE *fout = fopen("out.dat", "w");

	printf("%i\n", ioctl(fd, DATADEV_IOCT_GET));
	ioctl(fd, DATADEV_IOCT_SET, DATA_LEECH);
	printf("%i\n", ioctl(fd, DATADEV_IOCT_GET));

	int frame_size = ioctl(fd, DATADEV_IOCT_QUERY, QUERY_FRAMESIZE);
		
	while (1) {
		int ok = ioctl(fd, DATADEV_IOCT_QUERY, QUERY_LVALID);
		if (!ok) {
			ioctl(fd, DATADEV_IOCT_RESET);
			frame_size = ioctl(fd, DATADEV_IOCT_QUERY,
					   QUERY_FRAMESIZE);
			printf("New acq detected, new frame size %i.\n", 
				frame_size);
		}
		printf("%i:%i   %i|%i:%i\n",
		       ioctl(fd, DATADEV_IOCT_QUERY, QUERY_TAIL),
		       ioctl(fd, DATADEV_IOCT_QUERY, QUERY_PARTIAL),
		       ok,
		       ioctl(fd, DATADEV_IOCT_QUERY, QUERY_LTAIL),
		       ioctl(fd, DATADEV_IOCT_QUERY, QUERY_LPARTIAL));
		char data[5000];
		int count = read(fd, data, 5000);
		if (count > 0) {
			printf("Read %i data.\n", count);
			fwrite(data, 1, count, fout);
			fflush(fout);
		} else if (count < 0) {
			printf("Read error %i, %i\n", count, errno);
			if (errno==ENODATA) {
				printf(" Requeuing...\n");
				ioctl(fd, DATADEV_IOCT_EMPTY);
			}
		}
		usleep(100000);
	}
	close(fd);

	return 0;
}