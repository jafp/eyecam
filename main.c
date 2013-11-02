#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <assert.h>	
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "common.h"
#include "configuration.h"
#include "i2c.h"
#include "broadcast.h"
#include "motor_ctrl.h"
#include "camera.h"
#include "log.h"


/**
 * Image size
 */
#define WIDTH 					320
#define HEIGHT 					240
#define IMG_SIZE 				(WIDTH * HEIGHT)

/** 
 * Image processing
 */
#define FLOOR					255
#define LINE					0


/**
 * Overall states for the state machine.
 */
typedef enum {
	WAITING,
	GOTO_LINE,
	FOLLOW_LINE,
	FOLLOW_LINE_SPEEDY,
	GOTO_WALL_AND_BACK,
	FOLLOW_WALL,
	TRACK_COMPLETE
} state_t;

typedef enum {
	GO_STRAIGTH,
	TURN_RIGHT,
	READY
} goto_line_state_t;

/**
 * Information about a `slice` of the image, 
 * including mass of the line, error and so on.
 */
typedef struct slice {
	int x, y, mass, error;
} slice_t;

/**
 * List of log entries
 */
static log_list_t * logs;

/**
 * Camera interface (see cam.h)
 */
static camera_t * cam;

/**
 * Buffer for temporary image data
 */ 
static unsigned char * buffer;

/**
 * Frame counter
 */
static unsigned long frame_counter;

/**
 * Current state.
 */
static state_t current_state;

static goto_line_state_t goto_line_state;

/** 
 * Variables used in the PID controller.
 */
static int speed_ref;
static int speed_l;
static int speed_r;
static float last_error = 0;
static int I_sum = 0;
static float k_p, k_i, k_d, k_error, k_error_diff;
static float k_constrast, k_brightness;
static int slice_upper_start, slice_upper_end, slice_lower_start, slice_lower_end;
static int thr_enable, thr_lower, thr_upper;


static int cnt;

/**
 * Function prototypes
 */
static int update_loop(int mass, slice_t * upper, slice_t * lower);


/**
 * Calculate the "center of mass" of the given portion of the image,
 * given as an Y-offset and Y-length.
 */
static void calculate_center_of_mass(slice_t * pt, int y_offset_start, int y_offset_end)
{
	int offset_start, offset_end, sum = 0, x = 0, y = 0, i;
	pt->x = pt->y = pt->error = 0;

	offset_start = y_offset_start * WIDTH;
	offset_end = y_offset_end * WIDTH;

	for (i = offset_start; i < offset_end; i++)
	{
		if (buffer[i] == LINE)
		{
			x += i % WIDTH;
			y += i / WIDTH;
			sum++;
		}
	}

	if (sum > 0)
	{
		pt->x = x / sum;
		pt->y = y / sum;
		pt->error = (WIDTH / 2) - pt->x;
	}

	pt->mass = sum;
}

static void extract_slice(int start, int end, int peak_split, int nice)
{
	int i;
	int min, min_idx;
	int histogram[256] = {0};
	int peak_a_idx = 0, peak_a_max = 0;
	int peak_b_idx = 0, peak_b_max = 0;

	start = start * WIDTH;
	end = end * WIDTH;


	for (i = start; i < end; i++)
	{
		histogram[buffer[i]]++;
	}

	// Find peaks
	for (i = 0; i < peak_split; i++)
	{
		if (histogram[i] > peak_a_max) 
		{
			peak_a_max = histogram[i];
			peak_a_idx = i;
		}
	}

	for (i = peak_split; i < 255; i++)
	{
		if (histogram[i] > peak_b_max) 
		{
			peak_b_max = histogram[i];
			peak_b_idx = i;
		}
	}

	min = peak_a_max, min_idx = 0;

	for (i = peak_a_idx; i < peak_b_idx; i++)
	{
		if (histogram[i] < min)
		{
			min = histogram[i];
			min_idx = i;
		}
	}			

	for (i = start; i < end; i++)
	{
		buffer[i] = buffer[i] < (min_idx + nice) ? LINE : FLOOR;
	}
}	


/** 
 * 
 */
static void extract_line()
{
	extract_slice(0, 48, 100, 0);
	extract_slice(48, 88, 100, 0);
	extract_slice(88, 144, 100, 0);
	extract_slice(144, 192, 100, 0);
	extract_slice(192, 240, 100, 0);
}


/**
 * Callback fired when a frame is ready.
 * 
 * \param cam Pointer to the current camera context
 * \param frame Pointer to the frame data (planar image data)
 * \param length The of the frame data (in bytes, not pixels)
 */
static void frame_callback(struct camera * cam, void * frame, int length)
{
	int i;
	unsigned char * ptr;
	unsigned int count = 0;
	slice_t lower, upper;

	ptr = (unsigned char *) frame;

	// TODO UPDATE!!
	//
	// Image processing part of the code!
	//
	// The following steps are done:
	//  - Every pixel below a curtain gray scale value are "marked" as the line,
	//    and the rest of the pixels are marked as the floor. The line pixels
	//	  are colored black, and the rest white.
	//
	//  - The X and Y values of each pixel is added together, and the total
	// 	  number of "line pixels" are counted.
	//
	//  - ...

	// Copy to working buffer
	for (i = 0; i < IMG_SIZE; i++)
	{
		buffer[i] = (*ptr);
		ptr += 2;
	}

	// Extract line
	extract_line();

	// 
	// Calculate center of mass at the upper half of the image.
	// (this is where the line is farest away)
	//
	calculate_center_of_mass(&upper, slice_upper_start, slice_upper_end);

	//
	// Calculate center of mass at the lower half of the image
	//
	calculate_center_of_mass(&lower, slice_lower_start, slice_lower_end);

	// Aggregated mass of line
	count = upper.mass + lower.mass;

	// Transmit every 4rd frame over sockets.
	// This is 15 frames per second when we are capturing
	// 60 frames per second from the camera.
	if (frame_counter % 3 == 0)
	{
		//printf("%d %d -- %d %d\n", lower.x, lower.y, upper.x, upper.y);
		broadcast_send(lower.x, lower.y, upper.x, upper.y, lower.error, 
			upper.error, count, buffer);
	}

	frame_counter++;

	// Dispatch the updating to another function
	update_loop(count, &upper, &lower);
}

/**
 *
 */
static unsigned char limit_speed(int speed)
{	
	if (speed > 255)
	{
		return 255;
	}
	else if (speed < 0)
	{
		return 0;
	}
	return speed;
}

/**
 * Update loop callback. Called whenever a new image has been processed.
 * From this point, it's all about calculating new speeds for the motors,
 * and transmitting the updates to them.
 * 
 * \param error The error calculated from the image (diff. in X coordinate)
 * \param x The calculated X position of the center mass of the line
 * \param x The calculated Y position of the center mass of the line
 * \param mass The number of pixels identified as the line
 */
static int update_loop(int mass, slice_t * upper, slice_t * lower)
{
	switch (current_state)
	{
		case FOLLOW_LINE:
		case FOLLOW_LINE_SPEEDY:
		{
			float P, I, D;
			float err;
			float correction;
			float err_diff;
			float mass_pct;
			float mass_limiter = 0.0;
			unsigned char tacho_left = 0, tacho_right = 0;

			// Initial speed reference
			//speed_ref = speed;

			if (current_state == FOLLOW_LINE_SPEEDY)
			{
				// TODO Increase speed!
			}

			// Percentage of the image which is identified as line (mass)
			mass_pct = mass / IMG_SIZE * 100;
			//if (mass_pct > 60.0)7
			if (mass > 22000 && mass <  30000)
			{
				// Increase speed!!
				printf("INCREASE!!\n");
				//speed_ref = 70;
			}

			if (mass > 30000)
			{
				printf("WAIT!!\n");
				//current_state = GOTO_WALL_AND_BACK;
				//motor_ctrl_set_speed(0, 0);
				//cnt = 0;
				return;
			}

			// Get speed... We don't actually use it
			// motor_ctrl_get_speed(&tacho_left, &tacho_right);

			// Scale error down
			err = lower->error * k_error;
			
			//
			// Calculate PID
			//
			P = err * k_p;
 
			I_sum = 0.5 * I_sum + err;
			I = I_sum * k_i;

			D = (err - last_error) * k_d;
			last_error = err;

			// Total correction
			correction = P + I + D;

			// Limit speed if the line has big changes in direction 
			// in the future
			err_diff = abs(lower->error - upper->error) * k_error_diff;

			// Calculate new speed
			speed_l = (int) round(speed_ref - err_diff - mass_limiter - correction);
			speed_r = (int) round(speed_ref - err_diff - mass_limiter + correction);

			// Send new speeds to motor controller
			// (Each speed is limited to the interval 0-255 (unsigned 8-bit number))
			motor_ctrl_set_speed(limit_speed(speed_l), limit_speed(speed_r));

			//
			// Add log entry
			//

			log_entry_t * entry = log_entry_create();
			log_fields_t * f = &entry->fields;

			f->time = 0;
			f->frame = frame_counter;

			f->error_lower_x = lower->error;
			f->error_upper_x = upper->error;
			f->mass = mass;

			f->P = P;
			f->I = I;
			f->D = D;
			f->correction = correction;

			f->speed_left = speed_l;
			f->speed_right = speed_r;

			f->speed_ref_left = speed_ref;
			f->speed_ref_right = speed_ref;

			f->tacho_left = (int) tacho_left;
			f->tacho_right = (int) tacho_right;

			log_add(logs, entry);	

			break;
		}
		case GOTO_LINE:
		{
			switch (goto_line_state)
			{
				// Go straight till we see the line
				case GO_STRAIGTH:
				{
					if (upper->mass > 5000)
					{
						motor_ctrl_set_speed(0, 0);
						goto_line_state = TURN_RIGHT;
					}
					else
					{
						motor_ctrl_set_speed(30, 30);
					}
					break;
				}
				// The is perpendicular in front of us.
				// Turn right till the robot is parallel on top of the line
				case TURN_RIGHT:
				{
					if (lower->mass > 8000)
					{
						motor_ctrl_set_speed(0, 0);
						goto_line_state = READY;
					}
					else
					{
						motor_ctrl_set_speed(30, 0);
					}
					break;
				}
				// We are know ready to follow the line
				case READY:
				{
					current_state = FOLLOW_LINE;
					break;
				}
			}

			break;
		}
		case GOTO_WALL_AND_BACK:
		{
			cnt++;
			if (cnt > 100)
			{
				current_state = FOLLOW_LINE;
			}
			break;
		}
		default: 
		{

		}
	}
}

/*
static void print_ctrl_constants()
{
	printf("K_p = %.2f, K_i = %.2f, K_d = %.2f\n"
			"K_error = %.2f, K_error_diff = %.2f\n"
			"speed = %d\n", 
			K_p, K_i, K_d, K_error, K_error_diff, speed_ref);
}
*/

static void load_config()
{
	config_reload();

	speed_ref = config_get_int("speed");

	k_p = config_get_float("k_p");
	k_i = config_get_float("k_i");
	k_d = config_get_float("k_d");
	k_error = config_get_float("k_error");
	k_error_diff = config_get_float("k_error_diff");

	k_constrast = config_get_float("k_constrast");
	k_brightness = config_get_float("k_brightness");

	slice_upper_start = config_get_int("slice_upper_start");
	slice_upper_end = config_get_int("slice_upper_end");

	slice_lower_start = config_get_int("slice_lower_start");
	slice_lower_end = config_get_int("slice_lower_end");

	thr_enable = config_get_int("thr_enable");
	thr_upper = config_get_int("thr_upper");
	thr_lower = config_get_int("thr_lower");
}

/**
 * SIGINT signal handler.
 * 
 * \param signal Signal number
 */
static void sigint_handler(int signal)
{
	//cam_end_loop(cam);
	current_state = WAITING;
}

/**
 *
 */
static void * processing_thread_fn(void * ptr)
{
	cam_loop(cam);
	pthread_exit(0);
}

/**
 *
 */
static void * shell_thread_fn(void * ptr)
{
	int i;
	char buffer[255];

	while (1)
	{	
		printf(" >> ");
		if (fgets(buffer, 255, stdin) != NULL)
		{	
			// Skip empty line
			if (buffer[0] == '\n')
			{
				continue;
			}

			// Remove newline at the end
			for (i = 0; i < 255; i++)
			{
				if (buffer[i] == '\0' || buffer[i] == '\n')
				{
					buffer[i] = '\0';
				}
			}

			/**
			 *
			 */
			if (strcmp(buffer, "st") == 0)
			{	
				// Start motor at the initial speed
				I_sum = 0;
				motor_ctrl_set_speed(speed_l, speed_r);

				current_state = FOLLOW_LINE;
			}
			/**
			 *
			 */
			else if (strcmp(buffer, "s") == 0)
			{
				motor_ctrl_set_speed(0, 0);
				current_state = WAITING;
			}
			
			else if (strcmp(buffer, "r") == 0)
			{
				load_config();
				printf("Constants reloaded\n");
			}
			/**
			 *
			 */
			else if (strcmp(buffer, "exit") == 0)
			{
				cam_end_loop(cam);
				pthread_exit(0);
			}
			
			/**
			 *
			 */
			else if (strcmp(buffer, "goto") == 0)
			{
				current_state = GOTO_LINE;
			}
			else
			{
				printf("Command not found!\n");
			}
		}
	}
}

static void setup_camera()
{
	cam = (struct camera *) malloc(sizeof(struct camera));
	if (cam == NULL)
	{
		printf("Could not allocate memory for camera interface, exiting...\n");
		exit(-1);
	}

	// Camera configuration
	cam->config.frame_cb = frame_callback;
	cam->config.width = WIDTH;
	cam->config.height = HEIGHT;
	cam->config.fps = config_get_int("fps");
	cam->dev = config_get_str("device");
}


/**
 * Main entry point.
 */
int main(int argc, char ** argv)
{
	pthread_t processing_thread, shell_thread;
	struct addrinfo hints, *res;

	// Initialize variables
	frame_counter = 0;
	current_state = WAITING;
	buffer = (unsigned char *) malloc(IMG_SIZE);

	// Init and load the configuration file
	config_init();
	load_config();

	// Allocate and initialize the logging system
	logs = log_create();

	// Setup camera
	setup_camera();	

	// Open i2c bus (by internally opening the i2c device driver)
	if (i2c_bus_open() < 0)
	{
		printf("Failed opening I2C bus, exiting...\n");
		exit(-1);
	}

	// Print a nice welcome message
	printf("\n=== Eyecam ===\n");
	printf("Config: w: %d, h: %d, fps (expected): %d\n", cam->config.width, cam->config.height, cam->config.fps);

	// Catch CTRL-C signal and end looping
	signal(SIGINT, sigint_handler);

	// Open camera and start capturing
	cam_init(cam);
	cam_start_capturing(cam);
	
	// Open TCP server socket, and start listening for connections	
	broadcast_init();
	broadcast_start();

	// Create the main processing thread (camera and update loop)
	pthread_create(&processing_thread, NULL, processing_thread_fn, NULL);

	// Create the shell thread
	pthread_create(&shell_thread, NULL, shell_thread_fn, NULL);
	
	// Wait for the main thread and shell thread to finish
	pthread_join(processing_thread, NULL);
	pthread_join(shell_thread, NULL);	
	
	cam_stop_capturing(cam);
	cam_uninit(cam);

	// Stop robot
	motor_ctrl_set_speed(0, 0);

	// Close server socket and drop connections
	broadcast_release();

	//
	// Dump log information to CSV file
	//
	char filename[64];
	time_t now;
	struct tm * timeinfo;

	time(&now);
	timeinfo = localtime(&now);
	sprintf(filename, "run-%d-%d-%d-%d-%d-%d.csv", timeinfo->tm_mday, 
		timeinfo->tm_mon + 1, timeinfo->tm_year + 1900, timeinfo->tm_hour, 
		timeinfo->tm_min, timeinfo->tm_sec);

	//printf("Logging run data to %s\n", filename);
	//log_dump(logs, filename);

	// Print some statistics.
	// TODO: Calculate and show some statistics while running?
	printf("\nActual fps: %f\n", cam_get_measured_fps(cam));
	printf("Done.\n\n");

	return 0;
}

