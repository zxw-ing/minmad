/*
 * minmad - a minimal mp3 player using libmad and oss
 *
 * Copyright (C) 2009-2011 Ali Gholami Rudi
 *
 * This program is released under GNU GPL version 2.
 */
#include <ctype.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/soundcard.h>
#include <mad.h>

#define CTRLKEY(x)		((x) - 96)
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#define JMP3		600
#define JMP2		60
#define JMP1		10

static int afd;
static unsigned char *mem;
static unsigned long len;
static struct mad_decoder decoder;
static unsigned long pos;
static int fsize;		/* frame size */
static int flen;		/* frame duration in milliseconds */
static int count;
static mad_timer_t played;
static unsigned int rate;

static struct termios termios;
static int exited;
static int paused;

static int readkey(void)
{
	char b;
	if (read(STDIN_FILENO, &b, 1) <= 0)
		return -1;
	return b;
}

static void updatepos(void)
{
	if (decoder.sync) {
		pos = decoder.sync->stream.this_frame - mem;
		fsize = decoder.sync->stream.next_frame -
			decoder.sync->stream.this_frame;
		flen = mad_timer_count(decoder.sync->frame.header.duration,
					MAD_UNITS_MILLISECONDS);
	}
}

static void printinfo(void)
{
	int per = pos * 1000.0 / len;
	int dur = mad_timer_count(played, MAD_UNITS_DECISECONDS);
	int loc = pos * flen / fsize / 1000;
	printf("minmad:   %3d:%02d   %3d.%d%%   %8d.%ds\r",
		loc / 60, loc % 60,
		per / 10, per % 10, dur / 10, dur % 10);
	fflush(stdout);
}

static int getcount(int def)
{
	int result = count ? count : def;
	count = 0;
	return result;
}

static void seek(int n)
{
	int diff = n * fsize * 1000 / (flen ? flen : 40);
	pos = MAX(0, MIN(len, pos + diff));
}

static void seek_thousands(int n)
{
	pos = len * (float) n / 1000;
	pos -= pos % fsize;
}

static int execkey(void)
{
	int c;
	updatepos();
	while ((c = readkey()) != -1) {
		switch (c) {
		case 'J':
			seek(JMP3 * getcount(1));
			return 1;
		case 'K':
			seek(-JMP3 * getcount(1));
			return 1;
		case 'j':
			seek(JMP2 * getcount(1));
			return 1;
		case 'k':
			seek(-JMP2 * getcount(1));
			return 1;
		case 'l':
			seek(JMP1 * getcount(1));
			return 1;
		case 'h':
			seek(-JMP1 * getcount(1));
			return 1;
		case '%':
			seek_thousands(getcount(0) * 10);
			return 1;
		case 'i':
			printinfo();
			break;
		case 'p':
		case ' ':
			paused = !paused;
			return 1;
		case 'q':
			exited = 1;
			return 1;
		case 27:
			count = 0;
			break;
		default:
			if (isdigit(c))
				count = count * 10 + c - '0';
		}
	}
	return 0;
}

static enum mad_flow input(void *data, struct mad_stream *stream)
{
	static unsigned long cpos;
	if (pos && pos == cpos) {
		exited = 1;
		return MAD_FLOW_STOP;
	}
	cpos = pos;
	mad_stream_buffer(stream, mem + pos, len - pos);
	return MAD_FLOW_CONTINUE;
}

static signed int scale(mad_fixed_t sample)
{
	/* round */
	sample += (1L << (MAD_F_FRACBITS - 16));
	/* clip */
	if (sample >= MAD_F_ONE)
		sample = MAD_F_ONE - 1;
	else if (sample < -MAD_F_ONE)
		sample = -MAD_F_ONE;
	/* quantize */
	return sample >> (MAD_F_FRACBITS + 1 - 16);
}

static void push_sample(char *buf, mad_fixed_t sample)
{
	*buf++ = (sample >> 0) & 0xff;
	*buf++ = (sample >> 8) & 0xff;
}

static void oss_conf(void)
{
	int ch = 2;
	int bits = 16;
	ioctl(afd, SOUND_PCM_WRITE_RATE, &rate);
	ioctl(afd, SOUND_PCM_WRITE_CHANNELS, &ch);
	ioctl(afd, SOUND_PCM_WRITE_BITS, &bits);
}

static char mixed[1 << 20];
static enum mad_flow output(void *data,
			 struct mad_header const *header,
			 struct mad_pcm *pcm)
{
	int i;
	int right = pcm->channels > 1 ? 1 : 0;
	mad_timer_add(&played, decoder.sync->frame.header.duration);
	for (i = 0; i < pcm->length; i++) {
		push_sample(mixed + i * 4, scale(pcm->samples[0][i]));
		push_sample(mixed + i * 4 + 2, scale(pcm->samples[right][i]));
	}
	if (header->samplerate != rate) {
		rate = header->samplerate;
		oss_conf();
	}
	write(afd, mixed, pcm->length * 4);
	return execkey() ? MAD_FLOW_STOP : MAD_FLOW_CONTINUE;
}

static enum mad_flow error(void *data,
				struct mad_stream *stream,
				struct mad_frame *frame)
{
	return MAD_FLOW_CONTINUE;
}

static void waitkey(void)
{
	struct pollfd ufds[1];
	ufds[0].fd = STDIN_FILENO;
	ufds[0].events = POLLIN;
	poll(ufds, 1, -1);
}

static void decode(void)
{
	mad_decoder_init(&decoder, NULL, input, 0, 0, output, error, 0);
	while (!exited) {
		if (paused) {
			waitkey();
			execkey();
		} else {
			mad_decoder_run(&decoder, MAD_DECODER_MODE_SYNC);
		}
	}
	mad_decoder_finish(&decoder);
}

static void oss_init(void)
{
	afd = open("/dev/dsp", O_RDWR);
	if (afd < 0) {
		fprintf(stderr, "cannot open /dev/dsp\n");
		exit(1);
	}
}

static void oss_close(void)
{
	close(afd);
}

static void term_setup(void)
{
	struct termios newtermios;
	tcgetattr(STDIN_FILENO, &termios);
	newtermios = termios;
	newtermios.c_lflag &= ~ICANON;
	newtermios.c_lflag &= ~ECHO;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &newtermios);
	fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
}

static void term_cleanup(void)
{
	tcsetattr(STDIN_FILENO, 0, &termios);
}

static void sigcont(int sig)
{
	term_setup();
}

int main(int argc, char *argv[])
{
	struct stat stat;
	int fd;
	if (argc < 2)
		return 1;
	fd = open(argv[1], O_RDONLY);
	if (fstat(fd, &stat) == -1 || stat.st_size == 0)
		return 1;
	mem = mmap(0, stat.st_size, PROT_READ, MAP_SHARED, fd, 0);
	len = stat.st_size;
	if (mem == MAP_FAILED)
		return 1;
	term_setup();
	signal(SIGCONT, sigcont);
	oss_init();
	decode();
	oss_close();
	term_cleanup();
	munmap(mem, stat.st_size);
	close(fd);
	return 0;
}
