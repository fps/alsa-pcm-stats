#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <sys/mman.h>

#include <string>
#include <boost/program_options.hpp>
#include <iostream>
#include <vector>

int period_size_frames;
int num_periods;
int sampling_rate_hz;
std::string pcm_device_name;
std::string sample_format;
int priority;
int buffer_size;
int sample_size;
int availability_threshold;
int frame_read_write_limit;
int poll_in_out;
int verbose;
int show_header;

typedef int32_t sample_t;
// typedef int16_t sample_t;
sample_t *buffer;

struct data {
    int playback_available;
    int capture_available;
    struct timespec wakeup_time;
    int poll_pollin;
    int poll_pollout;
    int playback_written;
    int capture_read;
    
    data() :
        playback_available(0),
        capture_available(0),
        wakeup_time{0},
        poll_pollin(0),
        poll_pollout(0),
        playback_written(0),
        capture_read(0) {
    
    }
};


int setup_pcm_device(snd_pcm_t *pcm);

int main(int argc, char *argv[]) {
    namespace po = boost::program_options;

    po::options_description options_desc("Options");
    options_desc.add_options()
        ("help,h", "produce this help message")
        ("verbose,v", po::value<int>(&verbose)->default_value(0), "whether to be a little more verbose")
        ("period-size,p", po::value<int>(&period_size_frames)->default_value(1024), "period size (audio frames)")
        ("number-of-periods,n", po::value<int>(&num_periods)->default_value(2), "number of periods")
        ("rate,r", po::value<int>(&sampling_rate_hz)->default_value(48000), "sampling rate (hz)")
        ("pcm-device-name,d", po::value<std::string>(&pcm_device_name)->default_value("default"), "the ALSA pcm device name string")
        ("priority,P", po::value<int>(&priority)->default_value(70), "SCHED_FIFO priority")
        ("availability-threshold,a", po::value<int>(&availability_threshold)->default_value(-1), "the number of frames available for capture or playback used to determine when to read or write to pcm stream (-1 means a period size)")
        ("frame-read-write-limit,l", po::value<int>(&frame_read_write_limit)->default_value(-1), "limit for the number of frames written/read during a single read/write (-1 means a period-size)")
        ("sample-size,s", po::value<int>(&sample_size)->default_value(1000), "the number of samples to collect for stats (might be less due how to alsa works)")
        ("wait-for-poll-in-out,w", po::value<int>(&poll_in_out)->default_value(1), "whether to wait for POLLIN/POLLOUT")
        ("sample-format,f", po::value<std::string>(&sample_format)->default_value("S32LE"), "the sample format. Available formats: S16LE, S32LE")
        ("show-header,o", po::value<int>(&show_header)->default_value(1), "whether to show a header in the output table")
    ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, options_desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout << options_desc << "\n";
        return EXIT_SUCCESS;
    }

    if (frame_read_write_limit == -1) {
        frame_read_write_limit = period_size_frames;
    }

    if (availability_threshold == -1) {
        availability_threshold = period_size_frames;
    }

    // 2 because stereo
    buffer_size = 2 * num_periods * period_size_frames;

    buffer = new sample_t[buffer_size];

    int ret;

    if (verbose) { fprintf(stderr, "locking memory...\n"); }
    ret = mlockall(MCL_FUTURE);
    if (ret != 0) {
        fprintf(stderr, "mlockall: %s\n", strerror(ret));
        return EXIT_FAILURE;
    }

    if (verbose) { fprintf(stderr, "setting SCHED_FIFO at priority: %d\n", priority); }

    // #################### scheduling and priority setup
    struct sched_param pthread_params;
    pthread_params.sched_priority = priority;
    ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &pthread_params);
    if (ret != 0) {
        fprintf(stderr, "setschedparam: %s\n", strerror(ret));
        return EXIT_FAILURE;
    }

    if (verbose) { fprintf(stderr, "opening alsa pcm devices...\n"); }

    // #################### alsa pcm device open
    snd_pcm_t *playback_pcm;
    ret = snd_pcm_open(&playback_pcm, pcm_device_name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_open: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }

    snd_pcm_t *capture_pcm;
    ret = snd_pcm_open(&capture_pcm, pcm_device_name.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_open: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }

    ret = setup_pcm_device(playback_pcm);
    if (ret != 0) {
        fprintf(stderr, "setup_pcm_device: %s\n", "Failed to setup playback device");
        return EXIT_FAILURE;
    }
    ret = setup_pcm_device(capture_pcm);
    if (ret != 0) {
        fprintf(stderr, "setup_pcm_device: %s\n", "Failed to setup capture device");
        return EXIT_FAILURE;
    }

    // #################### alsa pcm device linking
    ret = snd_pcm_link(playback_pcm, capture_pcm);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_link: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }

    // #################### alsa pcm device poll descriptors
    int playback_pfds_count = snd_pcm_poll_descriptors_count(playback_pcm);
    if (playback_pfds_count < 1) {
        fprintf(stderr, "poll descriptors count less than one\n");
        return EXIT_FAILURE;
    }

    int capture_pfds_count = snd_pcm_poll_descriptors_count(capture_pcm);
    if (capture_pfds_count < 1) {
        fprintf(stderr, "poll descriptors count less than one\n");
        return EXIT_FAILURE;
    }

    pollfd *pfds = new pollfd[capture_pfds_count + playback_pfds_count];
  
    std::vector<data> data_samples(sample_size);

    struct timespec;

    int sample_index = 0;

    if (verbose) { fprintf(stderr, "starting to sample...\n"); }

    while(true) {
        ret = snd_pcm_poll_descriptors(playback_pcm, pfds, playback_pfds_count);
        if (ret != playback_pfds_count) {
            fprintf(stderr, "wrong playback fd count\n");
            exit(EXIT_FAILURE);
        }

        ret = snd_pcm_poll_descriptors(capture_pcm, pfds+playback_pfds_count, capture_pfds_count);
        if (ret != capture_pfds_count) {
            fprintf(stderr, "wrong playback fd count\n");
            exit(EXIT_FAILURE);
        }


        ret = poll(pfds, playback_pfds_count + capture_pfds_count, 1000);
        if (ret < 0) {
            fprintf(stderr, "poll: %s\n", strerror(ret));
            break;
        }

        if (ret == 0) {
            fprintf(stderr, "poll timeout\n");
            break;
        }


        unsigned short revents = 0;

        ret = snd_pcm_poll_descriptors_revents(playback_pcm, pfds, playback_pfds_count, &revents);
        if (ret < 0) {
            fprintf(stderr, "snd_pcm_poll_descriptors_revents: %s\n", strerror(ret));
            break;
        }

        bool should_write = true;
        bool should_read = true;

        if (poll_in_out) {
            if (revents & POLLOUT) {
                data_samples[sample_index].poll_pollout = 1;
                should_write = true;
            } else {
                should_write = false;
            }
            
        }

        ret = snd_pcm_poll_descriptors_revents(capture_pcm, pfds + playback_pfds_count, capture_pfds_count, &revents);
        if (ret < 0) {
            fprintf(stderr, "snd_pcm_poll_descriptors_revents: %s\n", strerror(ret));
            break;
        }

        if (poll_in_out) {
            if (revents & POLLIN) {
                data_samples[sample_index].poll_pollin = 1;
                should_read = true;
            } else {
                should_read = false;
            }
        }

        int avail_playback = snd_pcm_avail_update(playback_pcm);
        int avail_capture = snd_pcm_avail_update(capture_pcm);

        clock_gettime(CLOCK_MONOTONIC, &data_samples[sample_index].wakeup_time);
        data_samples[sample_index].playback_available = avail_playback;
        data_samples[sample_index].capture_available = avail_capture;

        if (avail_playback < 0) {
            fprintf(stderr, "avail_playback: %s\n", snd_strerror(avail_playback));
            break;
        }

        if (avail_playback >= availability_threshold && should_write) {
            ret = snd_pcm_writei(playback_pcm, buffer, std::max(std::min(avail_playback, frame_read_write_limit), availability_threshold));
            data_samples[sample_index].playback_written = ret;
    
            if (ret < 0) {
                fprintf(stderr, "snd_pcm_writei: %s\n", snd_strerror(ret));
                break;
            }
        }

        if (avail_capture < 0) {
            fprintf(stderr, "avail_capture: %s\n", snd_strerror(avail_capture));
            break;
        }
    
        if (avail_capture >= availability_threshold && should_read){
            ret = snd_pcm_readi(capture_pcm, buffer, std::max(std::min(avail_capture, frame_read_write_limit), availability_threshold));
            data_samples[sample_index].capture_read = ret;
    
            if (ret < 0) {
                fprintf(stderr, "snd_pcm_readi: %s\n", snd_strerror(ret));
                break;
            }
        }

        ++sample_index;
        if (sample_index >= sample_size) {
            break;
        }
    }

    if (verbose) { fprintf(stderr, "done sampling...\n"); } 

    if (show_header) {
        printf("   tv.sec   tv.nsec available-playback available-capture POLLOUT POLLIN written    read\n");
    }
    for (int sample_index = 0; sample_index < sample_size; ++sample_index) {
        data data_sample = data_samples[sample_index];
        printf("%09ld %09ld %18d %17d %7d %6d %7d %7d\n", data_sample.wakeup_time.tv_sec, data_sample.wakeup_time.tv_nsec, data_sample.playback_available, data_sample.capture_available, data_sample.poll_pollout, data_sample.poll_pollin, data_sample.playback_written, data_sample.capture_read);
        if (data_sample.capture_available < 0 || data_sample.playback_available < 0) {
            break;
        }
    }

    // delete[] buffer;

    return EXIT_SUCCESS;
}

int setup_pcm_device(snd_pcm_t *pcm) {
    if (verbose) { fprintf(stderr, "setting up pcm device...\n"); }
    int ret = 0;

    // #################### alsa pcm device hardware parameters
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    ret = snd_pcm_hw_params_any(pcm, params);

    ret = snd_pcm_hw_params_set_channels(pcm, params, 2);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_channels: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }

    ret = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_access: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }

    if (sample_format == "S16LE") {
        ret = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    }
    else if (sample_format == "S32LE") {
        ret = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S32_LE);
    }
    else {
        fprintf(stderr, "unsupported sample format\n");
        return EXIT_FAILURE;
    }

    if (ret < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_format: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }

    ret = snd_pcm_hw_params_set_rate(pcm, params, sampling_rate_hz, 0);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_rate (%d): %s\n", sampling_rate_hz, snd_strerror(ret));
        return EXIT_FAILURE;
    }

    for (int index = 0; index < buffer_size; ++index) {
        buffer[index] = 0;
    }

    ret = snd_pcm_hw_params_set_buffer_size(pcm, params, period_size_frames * num_periods);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_buffer_size: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }

    ret = snd_pcm_hw_params_set_period_size(pcm, params, period_size_frames, 0);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_period_size (%d): %s\n", period_size_frames, snd_strerror(ret));
        return EXIT_FAILURE;
    }

    ret = snd_pcm_hw_params(pcm, params);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_hw_params: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }

    // #################### alsa pcm device software params
    snd_pcm_sw_params_t *sw_params;
    snd_pcm_sw_params_alloca(&sw_params);

    ret = snd_pcm_sw_params_current(pcm, sw_params);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_sw_params_current: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }


    ret = snd_pcm_sw_params_set_avail_min(pcm, sw_params, period_size_frames);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_sw_params_set_avail_min: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }

    // ret = snd_pcm_sw_params_set_start_threshold(pcm, sw_params, 0);
    ret = snd_pcm_sw_params_set_start_threshold(pcm, sw_params, period_size_frames);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_sw_params_set_start_threshold: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }

    snd_pcm_sw_params(pcm, sw_params);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_sw_params: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }

    if (verbose) { fprintf(stderr, "done.\n"); }
    return EXIT_SUCCESS;
}

