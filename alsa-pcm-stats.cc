#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#include <string>
#include <boost/program_options.hpp>
#include <iostream>
#include <vector>

int period_size_frames;
int num_periods;
int sampling_rate_hz;
std::string pcm_device_name;
int priority;
int buffer_size;
int sample_size;
int availability_threshold;
int poll_in_out;

typedef int32_t sample_t;
// typedef short sample_t;
sample_t *buffer;

struct data {
  int playback_available;
  int capture_available;
  struct timespec wakeup_time;
  int poll_pollin;
  int poll_pollout;

  data() :
	playback_available(-1),
	capture_available(-1),
    wakeup_time{0},
    poll_pollin(0),
    poll_pollout(0) {

  }
};


int setup_pcm_device(snd_pcm_t *pcm) {
  fprintf(stderr, "setting up pcm device...\n");
  int ret = 0;

  // #################### alsa pcm device hardware parameters
  snd_pcm_hw_params_t *params;
  snd_pcm_hw_params_alloca(&params);
  ret = snd_pcm_hw_params_any(pcm, params);

  ret = snd_pcm_hw_params_set_channels(pcm, params, 2);
  if (ret < 0) {
    printf("snd_pcm_hw_params_set_channels: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }

  ret = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (ret < 0) {
    printf("snd_pcm_hw_params_set_access: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }

  ret = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S32_LE);
  if (ret < 0) {
    printf("snd_pcm_hw_params_set_format: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }

  ret = snd_pcm_hw_params_set_rate(pcm, params, sampling_rate_hz, 0);
  if (ret < 0) {
    printf("snd_pcm_hw_params_set_rate (%d): %s\n", sampling_rate_hz, snd_strerror(ret));
    return EXIT_FAILURE;
  }


  for (int index = 0; index < buffer_size; ++index) {
    buffer[index] = 0;
  }

  ret = snd_pcm_hw_params_set_buffer_size(pcm, params, buffer_size);
  if (ret < 0) {
    printf("snd_pcm_hw_params_set_buffer_size: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }

  ret = snd_pcm_hw_params_set_period_size(pcm, params, period_size_frames, 0);
  if (ret < 0) {
    printf("snd_pcm_hw_params_set_period_size (%d): %s\n", period_size_frames, snd_strerror(ret));
    return EXIT_FAILURE;
  }

  ret = snd_pcm_hw_params(pcm, params);
  if (ret < 0) {
    printf("snd_pcm_hw_params: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }

  // #################### alsa pcm device software params
  snd_pcm_sw_params_t *sw_params;
  snd_pcm_sw_params_alloca(&sw_params);

  ret = snd_pcm_sw_params_current(pcm, sw_params);
  if (ret < 0) {
    printf("snd_pcm_sw_params_current: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }


  ret = snd_pcm_sw_params_set_avail_min(pcm, sw_params, period_size_frames);
  if (ret < 0) {
    printf("snd_pcm_sw_params_set_avail_min: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }

  // ret = snd_pcm_sw_params_set_start_threshold(pcm, sw_params, 0);
  ret = snd_pcm_sw_params_set_start_threshold(pcm, sw_params, period_size_frames);
  if (ret < 0) {
    printf("snd_pcm_sw_params_set_start_threshold: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }

  snd_pcm_sw_params(pcm, sw_params);
  if (ret < 0) {
    printf("snd_pcm_sw_params: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }

  fprintf(stderr, "done.\n");
  return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
  namespace po = boost::program_options;

  po::options_description options_desc("Options");
  options_desc.add_options()
    ("help,h", "produce this help message")
    ("period-size,p", po::value<int>(&period_size_frames)->default_value(1024), "period size (audio frames)")
    ("num-periods,n", po::value<int>(&num_periods)->default_value(2), "number of periods")
    ("rate,r", po::value<int>(&sampling_rate_hz)->default_value(48000), "sampling rate (hz)")
    ("pcm-device-name,d", po::value<std::string>(&pcm_device_name)->default_value("default"), "the ALSA pcm device name string")
    ("priority,P", po::value<int>(&priority)->default_value(70), "SCHED_FIFO priority")
    ("availability-threshold,a", po::value<int>(&availability_threshold)->default_value(1), "the numner of frames available for capture or playback used to determine when to read or write to pcm stream")
    ("sample-size,s", po::value<int>(&sample_size)->default_value(1000), "the number of samples to collect for stats (might be less due how to alsa works)")
    ("wait-for-poll-in-out,w", po::value<int>(&poll_in_out)->default_value(0), "whether to wait for POLLIN/POLLOUT")
  ;

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, options_desc), vm);
  po::notify(vm);
  
  if (vm.count("help")) {
    std::cout << options_desc << "\n";
    return EXIT_SUCCESS;
  }

  buffer_size = 2 * num_periods * period_size_frames;

  buffer = new sample_t[buffer_size];

  int ret;

  fprintf(stderr, "setting SCHED_FIFO at priority: %d\n", priority);

  // #################### scheduling and priority setup
  struct sched_param pthread_params;
  pthread_params.sched_priority = priority;
  ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &pthread_params);
  if (ret != 0) {
    printf("setschedparam: %s\n", strerror(ret));
    return EXIT_FAILURE;
  }

  fprintf(stderr, "opening alsa pcm devices...\n");

  // #################### alsa pcm device open
  snd_pcm_t *playback_pcm;
  ret = snd_pcm_open(&playback_pcm, pcm_device_name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
  if (ret < 0) {
    printf("snd_pcm_open: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }

  snd_pcm_t *capture_pcm;
  ret = snd_pcm_open(&capture_pcm, pcm_device_name.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
  if (ret < 0) {
    printf("snd_pcm_open: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }

  ret = setup_pcm_device(playback_pcm);
  if (ret != 0) {
    printf("setup_pcm_device: %s\n", "Failed to setup playback device");
    return EXIT_FAILURE;
  }
  ret = setup_pcm_device(capture_pcm);
  if (ret != 0) {
    printf("setup_pcm_device: %s\n", "Failed to setup capture device");
    return EXIT_FAILURE;
  }

  ret = snd_pcm_link(playback_pcm, capture_pcm);
  if (ret < 0) {
    printf("snd_pcm_link: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }


  /*
  fprintf(stderr, "waiting for pcm devices...\n");

  ret = snd_pcm_wait(playback_pcm, 1000);
  if (ret < 0) {
    printf("snd_pcm_wait: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }

  ret = snd_pcm_wait(capture_pcm, 1000);
  if (ret < 0) {
    printf("snd_pcm_wait: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }
  */

  // the device starts automatically once the start threshold is reached
  // ret = snd_pcm_start(pcm);
  // if (ret < 0) {
  //   printf("start: %s\n", snd_strerror(ret));
  //   return EXIT_FAILURE;
  // }

  // #################### alsa pcm device poll descriptors
  int playback_pfds_count = snd_pcm_poll_descriptors_count(playback_pcm);
  if (playback_pfds_count < 1) {
    printf("poll descriptors count less than one\n");
    return EXIT_FAILURE;
  }

  pollfd *playback_pfds = new  pollfd[playback_pfds_count];
  int filled_playback_pfds = snd_pcm_poll_descriptors(playback_pcm, playback_pfds, playback_pfds_count);



  int capture_pfds_count = snd_pcm_poll_descriptors_count(capture_pcm);
  if (capture_pfds_count < 1) {
    printf("poll descriptors count less than one\n");
    return EXIT_FAILURE;
  }

  pollfd *capture_pfds = new pollfd[capture_pfds_count];
  int filled_capture_pfds = snd_pcm_poll_descriptors(capture_pcm, capture_pfds, capture_pfds_count);



  pollfd *pfds = new pollfd[filled_capture_pfds + filled_playback_pfds];

  for (int index = 0; index < filled_playback_pfds; ++index) {
    pfds[index] = playback_pfds[index];
  }

  for (int index = 0; index < filled_capture_pfds; ++index) {
    pfds[index + filled_playback_pfds] = capture_pfds[index];
  }

  std::vector<data> data_samples(sample_size);

  struct timespec;

  int sample_index = 0;

  fprintf(stderr, "starting to sample...\n");

  while(true) {
    ret = poll(pfds, filled_playback_pfds+filled_capture_pfds, 1000);
    if (ret < 0) {
      fprintf(stderr, "poll: %s\n", strerror(ret));
			break;
    }

    if (ret == 0) {
      printf("poll timeout\n");
			break;
    }

    bool should_write = false;
    bool should_read = false;

    unsigned short revents;

    ret = snd_pcm_poll_descriptors_revents(playback_pcm, pfds, filled_playback_pfds, &revents);
    if (ret < 0) {
      fprintf(stderr, "snd_pcm_poll_descriptors_revents: %s\n", strerror(ret));
			break;
    }
 
    if (revents & POLLOUT) {
        data_samples[sample_index].poll_pollout = 1;
        if (poll_in_out) {
            should_write = true;
        }
    }

    ret = snd_pcm_poll_descriptors_revents(capture_pcm, pfds+filled_playback_pfds, filled_capture_pfds, &revents);
    if (ret < 0) {
      fprintf(stderr, "snd_pcm_poll_descriptors_revents: %s\n", strerror(ret));
			break;
    }

    if (revents & POLLIN) {
        data_samples[sample_index].poll_pollin = 1;
        if (poll_in_out) {
            should_read = true;
        }
    }

    snd_pcm_sframes_t avail_playback = snd_pcm_avail_update(playback_pcm);
    snd_pcm_sframes_t avail_capture = snd_pcm_avail_update(capture_pcm);

    clock_gettime(CLOCK_MONOTONIC, &data_samples[sample_index].wakeup_time);
    data_samples[sample_index].playback_available = avail_playback;
    data_samples[sample_index].capture_available = avail_capture;

    if (avail_playback < 0) {
      printf("avail_playback: %s\n", snd_strerror(avail_playback));
      break;
    }

    if (avail_playback >= availability_threshold) {
      // ret = snd_pcm_writei(playback_pcm, buffer, period_size_frames);
      ret = snd_pcm_writei(playback_pcm, buffer, avail_playback);

      if (ret < 0) {
        printf("snd_pcm_writei: %s\n", snd_strerror(ret));
				break;
      }
    }
 
    if (avail_capture < 0) {
      printf("avail_capture: %s\n", snd_strerror(avail_capture));
			break;
    }

    if (avail_capture >= availability_threshold) {
      // ret = snd_pcm_readi(capture_pcm, buffer, period_size_frames);
      ret = snd_pcm_readi(capture_pcm, buffer, avail_capture);

      if (ret < 0) {
        printf("snd_pcm_readi: %s\n", snd_strerror(ret));
				break;
      }
    }

    ++sample_index;
    if (sample_index >= sample_size) {
      break;
    }
  } 

  fprintf(stderr, "done sampling...\n"); 

  printf("   tv.sec   tv.nsec available-playback available-capture POLLOUT POLLIN\n");
  for (int sample_index = 0; sample_index < sample_size; ++sample_index) {
    data data_sample = data_samples[sample_index];
    printf("%09ld %09ld %018d %017d %07d %06d\n", data_sample.wakeup_time.tv_sec, data_sample.wakeup_time.tv_nsec, data_sample.playback_available, data_sample.capture_available, data_sample.poll_pollout, data_sample.poll_pollin);
  }

  // delete[] buffer;

  return EXIT_SUCCESS;
}
