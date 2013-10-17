
#ifndef _CAM_H_
#define _CAM_H_

#include <unistd.h>
#include <sys/time.h>

// Forward declaration of context structure
struct camera;

struct __buffer {
    void   *start;
    size_t  length;
};

struct cam_config {
	unsigned int width;
	unsigned int height;
	unsigned int fps;	
	void (*frame_cb)(struct camera *, void *, int length);
};

struct camera {
	struct cam_config config;

	int run;
	int fd;
	char * dev;
	struct __buffer * buffers;
	unsigned int n_buffers;

	struct timeval start, end;
	unsigned long frame_count;
};


void cam_init(struct camera *);
void cam_uninit(struct camera *);

void cam_start_capturing(struct camera *);
void cam_stop_capturing(struct camera *);

void cam_loop(struct camera *);
void cam_end_loop(struct camera *);

double cam_get_measured_fps(struct camera * ctx);

#endif
