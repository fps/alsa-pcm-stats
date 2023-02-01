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
int input_channels;
int output_channels;
int priority;
int buffer_size;
int sample_size;
int verbose;
int show_header;
int sleep_percent;

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
    int fill;
    
    data() :
        playback_available(0),
        capture_available(0),
        wakeup_time{0},
        poll_pollin(0),
        poll_pollout(0),
        playback_written(0),
        capture_read(0),
        fill(0) {
    
    }
};

int setup_pcm_device(snd_pcm_t *pcm, int channels);

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
        ("input-channels,i", po::value<int>(&input_channels)->default_value(2), "the number of input channels")
        ("output-channels,o", po::value<int>(&output_channels)->default_value(2), "the number of output channels")
        ("priority,P", po::value<int>(&priority)->default_value(70), "SCHED_FIFO priority")
        ("sample-size,s", po::value<int>(&sample_size)->default_value(1000), "the number of samples to collect for stats (might be less due how to alsa works)")
        ("sample-format,f", po::value<std::string>(&sample_format)->default_value("S32LE"), "the sample format. Available formats: S16LE, S32LE")
        ("show-header,e", po::value<int>(&show_header)->default_value(1), "whether to show a header in the output table")
        ("sleep,l", po::value<int>(&sleep_percent)->default_value(0), "the percentage of a period to sleep after reading a period")
    ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, options_desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout << options_desc << "\n";
        return EXIT_SUCCESS;
    }

    buffer_size = std::max(input_channels, output_channels) * num_periods * period_size_frames;

    buffer = new sample_t[buffer_size];
    for (int index = 0; index < buffer_size; ++index) {
        buffer[index] = 0;
    }

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

    // #################### alsa pcm device open
    if (verbose) { fprintf(stderr, "setting up playback device...\n"); }

    snd_pcm_t *playback_pcm;
    ret = snd_pcm_open(&playback_pcm, pcm_device_name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_open: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }

    ret = setup_pcm_device(playback_pcm, output_channels);
    if (ret != 0) {
        fprintf(stderr, "setup_pcm_device: %s\n", "Failed to setup playback device");
        return EXIT_FAILURE;
    }

    if (verbose) { fprintf(stderr, "setting up capture device...\n"); }

    snd_pcm_t *capture_pcm;
    ret = snd_pcm_open(&capture_pcm, pcm_device_name.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_open: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }

    ret = setup_pcm_device(capture_pcm, input_channels);
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

    std::vector<data> data_samples(sample_size);

    int sample_index = 0;

    if (verbose) { fprintf(stderr, "starting to sample...\n"); }

    int64_t written = 0;
    int fill = 0;

    // fill the whole playback buffer for a start
    int avail_playback = snd_pcm_avail_update(playback_pcm);

    if (avail_playback < 0) {
        fprintf(stderr, "avail_playback: %s\n", snd_strerror(avail_playback));
        return EXIT_FAILURE;
    }

    if (avail_playback != period_size_frames * num_periods) {
        fprintf(stderr, "no full buffer available\n");
        return EXIT_FAILURE;
    }

    ret = snd_pcm_writei(playback_pcm, buffer, period_size_frames * num_periods);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_writei: %s\n", snd_strerror(ret));
        return EXIT_FAILURE;
    }

    if (ret != period_size_frames * num_periods) {
        fprintf(stderr, "couldn't write a full buffer\n");
        return EXIT_FAILURE;
    }

    while(true) {
        usleep(10);
        data_samples[sample_index].fill = fill;

        clock_gettime(CLOCK_MONOTONIC, &data_samples[sample_index].wakeup_time);
        int avail_capture = snd_pcm_avail_update(capture_pcm);

        if (avail_capture < 0) {
            fprintf(stderr, "avail_capture: %s\n", snd_strerror(avail_capture));
            break;
        }
    
        if (avail_capture >= period_size_frames){
            ret = snd_pcm_readi(capture_pcm, buffer, period_size_frames);

            data_samples[sample_index].capture_read = ret;
    
            if (ret < 0) {
                fprintf(stderr, "snd_pcm_readi: %s\n", snd_strerror(ret));
                break;
            }

            fill += ret;

            timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 1e9f * ((float)sleep_percent/100.f) * ((float)period_size_frames / (float)sampling_rate_hz);
            nanosleep(&ts, NULL);
        }

        int avail_playback = snd_pcm_avail_update(playback_pcm);

        if (avail_playback < 0) {
            fprintf(stderr, "avail_playback: %s\n", snd_strerror(avail_playback));
            break;
        }

        if (avail_playback >= period_size_frames && fill >= period_size_frames) {
            ret = snd_pcm_writei(playback_pcm, buffer, period_size_frames);

            data_samples[sample_index].playback_written = ret;
    
            if (ret < 0) {
                fprintf(stderr, "snd_pcm_writei: %s\n", snd_strerror(ret));
                break;
            }

            written += ret;
            fill -= ret;

        }

        data_samples[sample_index].playback_available = avail_playback;
        data_samples[sample_index].capture_available = avail_capture;

        ++sample_index;
        if (sample_index >= sample_size) {
            break;
        }
    }

    if (verbose) { fprintf(stderr, "done sampling...\n"); } 

    if (show_header) {
        printf("   tv.sec   tv.nsec avail-w avail-r POLLOUT POLLIN written    read total-w total-r diff fill\n");
    }

    uint64_t total_written = 0;
    uint64_t total_read = 0;

    for (int sample_index = 0; sample_index < sample_size; ++sample_index) {
        data data_sample = data_samples[sample_index];
        total_written += data_sample.playback_written;
        total_read += data_sample.capture_read;
        printf("%09ld %09ld %7d %7d %7d %6d %7d %7d %7ld %7ld %4ld %4d\n", data_sample.wakeup_time.tv_sec, data_sample.wakeup_time.tv_nsec, data_sample.playback_available, data_sample.capture_available, data_sample.poll_pollout, data_sample.poll_pollin, data_sample.playback_written, data_sample.capture_read, total_written, total_read, total_read - total_written, data_sample.fill);
        if (data_sample.capture_available < 0 || data_sample.playback_available < 0) {
            break;
        }
    }

    // delete[] buffer;

    return EXIT_SUCCESS;
}

int setup_pcm_device(snd_pcm_t *pcm, int channels) {
    int ret = 0;

    // #################### alsa pcm device hardware parameters
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    ret = snd_pcm_hw_params_any(pcm, params);

    ret = snd_pcm_hw_params_set_channels(pcm, params, channels);
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

