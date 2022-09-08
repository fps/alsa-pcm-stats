#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <pthread.h>

#include <string>
#include <boost/program_options.hpp>
#include <iostream>
#include <vector>

int period_size_frames;
int num_periods;
int num_channels;
int sampling_rate_hz;
std::string pcm_device_name;
int priority;
int buffer_size;
unsigned short *buffer;

void setup_pcm_device(snd_pcm_t *pcm) {
  int ret;

  // #################### alsa pcm device hardware parameters
  snd_pcm_hw_params_t *params;
  snd_pcm_hw_params_alloca(&params);
  ret = snd_pcm_hw_params_any(pcm, params);

  ret = snd_pcm_hw_params_set_channels(pcm, params, 2);
  if (ret < 0) {
    printf("set channels: %s\n", snd_strerror(ret));
    exit(EXIT_FAILURE);
  }

  ret = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (ret < 0) {
    printf("set access: %s\n", snd_strerror(ret));
    exit(EXIT_FAILURE);
  }

  ret = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16);
  if (ret < 0) {
    printf("set format: %s\n", snd_strerror(ret));
    exit(EXIT_FAILURE);
  }

  ret = snd_pcm_hw_params_set_rate(pcm, params, sampling_rate_hz, 0);
  if (ret < 0) {
    printf("set rate (%d): %s\n", sampling_rate_hz, snd_strerror(ret));
    exit(EXIT_FAILURE);
  }


  for (int index = 0; index < buffer_size; ++index) {
    buffer[index] = 0;
  }

  ret = snd_pcm_hw_params_set_buffer_size(pcm, params, buffer_size);
  if (ret < 0) {
    printf("set buffer size: %s\n", snd_strerror(ret));
    exit(EXIT_FAILURE);
  }

  ret = snd_pcm_hw_params_set_period_size(pcm, params, period_size_frames, 0);
  if (ret < 0) {
    printf("set period size (%d): %s\n", period_size_frames, snd_strerror(ret));
    exit(EXIT_FAILURE);
  }

  ret = snd_pcm_hw_params(pcm, params);
  if (ret < 0) {
    printf("set hw params: %s\n", snd_strerror(ret));
    exit(EXIT_FAILURE);
  }

  // #################### alsa pcm device software params
  snd_pcm_sw_params_t *sw_params;
  snd_pcm_sw_params_alloca(&sw_params);

  ret = snd_pcm_sw_params_current(pcm, sw_params);
  if (ret < 0) {
    printf("sw params current: %s\n", snd_strerror(ret));
    exit(EXIT_FAILURE);
  }


  ret = snd_pcm_sw_params_set_avail_min(pcm, sw_params, period_size_frames);
  if (ret < 0) {
    printf("set avail min: %s\n", snd_strerror(ret));
    exit(EXIT_FAILURE);
  }

  ret = snd_pcm_sw_params_set_start_threshold(pcm, sw_params, period_size_frames);
  if (ret < 0) {
    printf("set start threshold: %s\n", snd_strerror(ret));
    exit(EXIT_FAILURE);
  }

  snd_pcm_sw_params(pcm, sw_params);
  if (ret < 0) {
    printf("sw params: %s\n", snd_strerror(ret));
    exit(EXIT_FAILURE);
  }

}

int main(int argc, char *argv[]) {
  namespace po = boost::program_options;

  po::options_description options_desc("Options");
  options_desc.add_options()
    ("help,h", "produce this help message")
    ("period-size,p", po::value<int>(&period_size_frames)->default_value(1024), "period size (audio frames)")
    ("num-periods,n", po::value<int>(&num_periods)->default_value(2), "number of periods")
    ("rate,r", po::value<int>(&sampling_rate_hz)->default_value(48000), "sampling rate (hz)")
    ("num-channels,c", po::value<int>(&num_channels)->default_value(1), "number of channels")
    ("pcm-device-name,d", po::value<std::string>(&pcm_device_name)->default_value("default"), "the ALSA pcm device name string")
    ("priority,P", po::value<int>(&priority)->default_value(70), "SCHED_FIFO priority")
  ;

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, options_desc), vm);
  po::notify(vm);
  
  if (vm.count("help")) {
    std::cout << options_desc << "\n";
    return EXIT_SUCCESS;
  }

  buffer_size = num_periods * period_size_frames;

  buffer = new unsigned short[buffer_size];

  int ret;

  // #################### scheduling and priority setup
  struct sched_param pthread_params;
  pthread_params.sched_priority = priority;
  ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &pthread_params);
  if (ret != 0) {
    printf("setschedparam: %s\n", strerror(ret));
    return EXIT_FAILURE;
  }

  // #################### alsa pcm device open
  snd_pcm_t *playback_pcm;
  ret = snd_pcm_open(&playback_pcm, pcm_device_name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
  if (ret < 0) {
    printf("failed to open playback device: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }

  snd_pcm_t *capture_pcm;
  ret = snd_pcm_open(&capture_pcm, pcm_device_name.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
  if (ret < 0) {
    printf("failed to open capture device: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }

  setup_pcm_device(playback_pcm);
  setup_pcm_device(capture_pcm);

  /*
  ret = snd_pcm_wait(playback_pcm, 1000);
  if (ret < 0) {
    printf("wait: %s\n", snd_strerror(ret));
    return EXIT_FAILURE;
  }

  ret = snd_pcm_wait(capture_pcm, 1000);
  if (ret < 0) {
    printf("wait: %s\n", snd_strerror(ret));
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
  int pfds_count = snd_pcm_poll_descriptors_count(playback_pcm);
  if (pfds_count < 1) {
    printf("poll descriptors count less than one\n");
    return EXIT_FAILURE;
  }

  struct pollfd pfds[pfds_count];
  int num_pfds = snd_pcm_poll_descriptors(playback_pcm, pfds, pfds_count);

  for (int index = 0; index < 1000000; ++index)  {
    snd_pcm_sframes_t avail = snd_pcm_avail_update(playback_pcm);
    printf("avail: %d\n", (int)avail);

    if (avail < 0) {
      printf("avail: %s\n", snd_strerror(avail));
      // return EXIT_FAILURE;
      ret = snd_pcm_prepare(playback_pcm);
      if (ret < 0) {
        printf("pcm prepare: %s\n", snd_strerror(ret));
      }
      continue;
    }

    if (avail >= period_size_frames) {
	  // ret = snd_pcm_readi(pcm, buffer, period_size_frames);
      ret = snd_pcm_writei(playback_pcm, buffer, period_size_frames);
      printf("written: %d\n", ret);
      if (ret < 0) {
        printf("writei: %s\n", snd_strerror(ret));
        ret = snd_pcm_prepare(playback_pcm);
        if (ret < 0) {
          printf("pcm prepare: %s\n", snd_strerror(ret));
        }
        continue;
      }
    }
 
    while (1) {
      ret = poll(pfds, num_pfds, 1000);
      if (ret < 0) {
        printf("poll: %s\n", strerror(ret));
      }

      if (ret == 0) {
        printf("poll timeout\n");
        break;
      }
  
      unsigned short revents;
      ret = snd_pcm_poll_descriptors_revents(playback_pcm, pfds, pfds_count, &revents);
      if (revents & POLLOUT) {
        break;
      }
      if (revents & POLLERR) {
        printf("poll failed\n");
        return EXIT_FAILURE;
      }
      printf("polling again\n");
    }
  } 

  return EXIT_SUCCESS;
}
