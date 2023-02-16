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
#include <sched.h>
#include <malloc.h>

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
int buffer_size_frames;
int sizeof_sample;
int sample_size;
int verbose;
int show_header;
int sleep_percent;
int busy_sleep_us;
int prefault_heap_size_mb;
int processing_buffer_frames;

uint8_t *input_buffer;
uint8_t *output_buffer;

float *ringbuffer;
int head = 0;
int tail = 0;

struct data {
    uint64_t cycles;
    int valid;
    int playback_available;
    int capture_available;
    struct timespec wakeup_time;
    int poll_pollin;
    int poll_pollout;
    int playback_written;
    int capture_read;
    int fill;
    int drain;
    
    data() :
        cycles(0),
        valid(0),
        playback_available(0),
        capture_available(0),
        wakeup_time{0},
        poll_pollin(0),
        poll_pollout(0),
        playback_written(0),
        capture_read(0),
        fill(0),
        drain(0) {
    
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
        ("busy,b", po::value<int>(&busy_sleep_us)->default_value(1), "the number of microseconds to sleep everytime when nothing was done")
        ("prefault-heap-size,a", po::value<int>(&prefault_heap_size_mb)->default_value(100), "the number of megabytes of heap space to prefault")
        ("processing-buffer-size,c", po::value<int>(&processing_buffer_frames)->default_value(-1), "the processing buffer size (audio frames)")
        ("load,l", po::value<int>(&sleep_percent)->default_value(0), "the percentage of a period to sleep after reading a period")
    ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, options_desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout << options_desc << "\n";
        exit(EXIT_SUCCESS);
    }

    int ret;

    if (verbose) { fprintf(stderr, "tuning memory allocator...\n"); }
    ret = mallopt(M_MMAP_MAX, 0);
    if (ret != 1) {
        fprintf(stderr, "mallopt M_MMAP_MAX: %s\n", strerror(ret));
        exit(EXIT_FAILURE);
    }

    ret = mallopt(M_TRIM_THRESHOLD, -1);
    if (ret != 1) {
        fprintf(stderr, "mallopt M_TRIM_THRESHOLD: %s\n", strerror(ret));
        exit(EXIT_FAILURE);
    }

    if (verbose) { fprintf(stderr, "locking memory...\n"); }
    ret = mlockall(MCL_CURRENT | MCL_FUTURE);
    if (ret != 0) {
        fprintf(stderr, "mlockall: %s\n", strerror(ret));
        exit(EXIT_FAILURE);
    }
  
    if (verbose) { fprintf(stderr, "prefaulting heap memory...\n"); }
    char *dummy_heap = (char*)malloc(1024 * 1024 * prefault_heap_size_mb);
    if (!dummy_heap) {
        fprintf(stderr, "failed to allocate prefaulting heap memory\n");
        exit(EXIT_FAILURE);
    }

    for (int index = 0; index < (1024 * 1024 * prefault_heap_size_mb); index += sysconf(_SC_PAGESIZE)) {
        dummy_heap[index] = 1;
    }

    free(dummy_heap);

    if (verbose) { fprintf(stderr, "prefaulting stack memory...\n"); }
    {
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wunused-but-set-variable"
        unsigned char dummy_stack[1024 * 1024];
        for (int index = 0; index < (1024 * 1024); index += sysconf(_SC_PAGESIZE)) {
            dummy_stack[index] = 1;
        }
        #pragma GCC diagnostic pop
    } 

    buffer_size_frames = num_periods * period_size_frames;
    // buffer_size_samples = std::max(input_channels, output_channels) * buffer_size_frames;

    if (2 * processing_buffer_frames > buffer_size_frames) {
        fprintf(stderr, "period-size * number-of-periods < 2 * processing-buffer-size.\n");
        exit(EXIT_FAILURE);
    }

    if (processing_buffer_frames == -1) processing_buffer_frames = period_size_frames;

    const int min_channels = std::min(input_channels, output_channels);
    const int sizeof_sample = (sample_format == "S16LE") ? 2 : 4;

    input_buffer = new uint8_t[buffer_size_frames * sizeof_sample * input_channels];
    for (int index = 0; index < buffer_size_frames * sizeof_sample * input_channels; ++index) {
        input_buffer[index] = 0;
    }

    output_buffer = new uint8_t[buffer_size_frames * sizeof_sample * output_channels];
    for (int index = 0; index < buffer_size_frames * sizeof_sample * output_channels; ++index) {
        output_buffer[index] = 0;
    }
  
    ringbuffer = new float[buffer_size_frames * min_channels];
    for (int index = 0; index < buffer_size_frames * min_channels; ++index) {
        ringbuffer[index] = 0;
    }


    if (verbose) { fprintf(stderr, "setting SCHED_FIFO at priority: %d\n", priority); }

    // #################### scheduling and priority setup
    struct sched_param pthread_params;
    pthread_params.sched_priority = priority;
    ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &pthread_params);
    if (ret != 0) {
        fprintf(stderr, "setschedparam: %s\n", strerror(ret));
        exit(EXIT_FAILURE);
    }

    // #################### alsa pcm device open
    if (verbose) { fprintf(stderr, "setting up playback device...\n"); }

    snd_pcm_t *playback_pcm;
    ret = snd_pcm_open(&playback_pcm, pcm_device_name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_open: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    ret = setup_pcm_device(playback_pcm, output_channels);
    if (ret != 0) {
        fprintf(stderr, "setup_pcm_device: %s\n", "Failed to setup playback device");
        exit(EXIT_FAILURE);
    }

    if (verbose) { fprintf(stderr, "setting up capture device...\n"); }

    snd_pcm_t *capture_pcm;
    ret = snd_pcm_open(&capture_pcm, pcm_device_name.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_open: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    ret = setup_pcm_device(capture_pcm, input_channels);
    if (ret != 0) {
        fprintf(stderr, "setup_pcm_device: %s\n", "Failed to setup capture device");
        exit(EXIT_FAILURE);
    }

    // #################### alsa pcm device linking
    ret = snd_pcm_link(playback_pcm, capture_pcm);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_link: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    std::vector<data> data_samples(sample_size);

    int sample_index = 0;

    if (verbose) { fprintf(stderr, "starting to sample...\n"); }

    int fill = 0;
    int drain = period_size_frames * num_periods;

    int avail_playback = snd_pcm_avail(playback_pcm);

    if (avail_playback < 0) {
        fprintf(stderr, "avail_playback: %s\n", snd_strerror(avail_playback));
        exit(EXIT_FAILURE);
    }

    if (avail_playback != drain) {
        fprintf(stderr, "no full buffer available\n");
        exit(EXIT_FAILURE);
    }


    while (drain > 0) {
        ret = snd_pcm_writei(playback_pcm, output_buffer, drain);
        if (ret < 0) {
            fprintf(stderr, "snd_pcm_writei: %s\n", snd_strerror(ret));
            exit(EXIT_FAILURE);
        }

        drain -= ret;
    }

    uint64_t cycles = 0;

    while(true) {

        data data_sample;

        snd_pcm_state_t state;

        state = snd_pcm_state(playback_pcm);
        if (state == SND_PCM_STATE_XRUN) {
            fprintf(stderr, "playback xrun\n");
            goto done;
        }

        state = snd_pcm_state(capture_pcm);
        if (state == SND_PCM_STATE_XRUN) {
            fprintf(stderr, "capture xrun\n");
            goto done;
        }
       

        clock_gettime(CLOCK_MONOTONIC, &data_sample.wakeup_time);
   
        // if (avail_capture > 0 && (fill < (num_periods * period_size_frames - avail_capture))) {
        if (fill < processing_buffer_frames) {
            int avail_capture = snd_pcm_avail(capture_pcm);

            if (avail_capture < 0) {
                fprintf(stderr, "avail_capture: %s. frame: %d\n", snd_strerror(avail_capture), sample_index);
                goto done;
            }

            data_sample.capture_available = avail_capture;

            if (avail_capture > 0) {
                int frames_to_read = std::min(processing_buffer_frames - fill, avail_capture);
                int frames_read = 0;
                while(frames_to_read != 0 && frames_read < frames_to_read) {
                    ret = snd_pcm_readi(capture_pcm, input_buffer + sizeof_sample * input_channels * frames_read, frames_to_read - frames_read);
    
                    if (ret < 0) {
                        fprintf(stderr, "snd_pcm_readi: %s. frame: %d\n", snd_strerror(ret), sample_index);
                        goto done;
                    }
                    frames_read += ret;
                }
    
                data_sample.capture_read = frames_read;
                fill += frames_read;

                for (int channel_index = 0; channel_index < min_channels; ++channel_index) {
                    for (int sample_index = 0; sample_index < frames_read; ++sample_index) {
                        switch(sizeof_sample) {
                            case 2:
                                ringbuffer[((head + sample_index) % buffer_size_frames) * min_channels + channel_index] = ((int16_t*)input_buffer)[sample_index * input_channels + channel_index] / (float)INT16_MAX;
                                break;
                            case 4:
                                ringbuffer[((head + sample_index) % buffer_size_frames) * min_channels + channel_index] = ((int32_t*)input_buffer)[sample_index * input_channels + channel_index] / (float)INT32_MAX;
                                break;
                        }
                    }
                }
                head = (head + frames_read) % buffer_size_frames;
            }
        }

        if (fill >= processing_buffer_frames) {
            timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 1e9f * ((float)sleep_percent/100.f) * ((float)processing_buffer_frames / (float)sampling_rate_hz);
            nanosleep(&ts, NULL);

            fill -= processing_buffer_frames;
            drain += processing_buffer_frames;
        }
 
        if (drain > 0) {
            avail_playback = snd_pcm_avail(playback_pcm);
    
            if (avail_playback < 0) {
                fprintf(stderr, "avail_playback: %s. frame: %d\n", snd_strerror(avail_playback), sample_index);
                goto done;
            }
    
            data_sample.playback_available = avail_playback;

            if (avail_playback > 0)  {
                int frames_to_write = std::min(drain, avail_playback);

                for (int channel_index = 0; channel_index < min_channels; ++channel_index) {
                    for (int sample_index = 0; sample_index < frames_to_write; ++sample_index) {
                        switch(sizeof_sample) {
                            case 2:
                                ((int16_t*)output_buffer)[sample_index * output_channels + channel_index] = INT16_MAX * ringbuffer[((tail + sample_index) % buffer_size_frames) * min_channels + channel_index];
                                break;
                            case 4:
                                ((int32_t*)output_buffer)[sample_index * output_channels + channel_index] = INT32_MAX * ringbuffer[((tail + sample_index) % buffer_size_frames) * min_channels + channel_index];
                                break;
                        }
                    }
                }
                tail = (tail + frames_to_write) % buffer_size_frames;
              

                int frames_written = 0;
                while (frames_written < frames_to_write) {
                    ret = snd_pcm_writei(playback_pcm, output_buffer + sizeof_sample * output_channels * frames_written, frames_to_write - frames_written);
                    frames_written += ret;

                    if (ret < 0) {
                        fprintf(stderr, "snd_pcm_writei: %s. frame: %d\n", snd_strerror(ret), sample_index);
                        goto done;
                    }
                }
                data_sample.playback_written = frames_written;
                drain -= frames_written;
            }
        }

        data_sample.cycles = cycles;

        ++cycles;

        if (data_sample.playback_written == 0 && data_sample.capture_read == 0) {
            usleep(busy_sleep_us);
            continue;
        }
  
        data_sample.drain = drain;
        data_sample.fill = fill;
        data_sample.valid = 1;

        data_samples[sample_index] = data_sample;

        ++sample_index;
        if (sample_index >= sample_size) {
            goto done;
        }
    }

    done: 

    if (verbose) { fprintf(stderr, "done sampling...\n"); } 

    if (show_header) {
        printf("   tv.sec   tv.nsec avail-w avail-r POLLOUT POLLIN written    read total-w total-r diff fill drain       cycles\n");
    }

    uint64_t total_written = 0;
    uint64_t total_read = 0;

    for (int sample_index = 0; sample_index < sample_size; ++sample_index) {
        data data_sample = data_samples[sample_index];
        total_written += data_sample.playback_written;
        total_read += data_sample.capture_read;
        printf("%09ld %09ld %7d %7d %7d %6d %7d %7d %7ld %7ld %4ld %4d %5d %12ld\n", data_sample.wakeup_time.tv_sec, data_sample.wakeup_time.tv_nsec, data_sample.playback_available, data_sample.capture_available, data_sample.poll_pollout, data_sample.poll_pollin, data_sample.playback_written, data_sample.capture_read, total_written, total_read, total_read - total_written, data_sample.fill, data_sample.drain, data_sample.cycles);
        if (!data_sample.valid) { break; }
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
        exit(EXIT_FAILURE);
    }

    ret = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_access: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    if (sample_format == "S16LE") {
        ret = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    }
    else if (sample_format == "S32LE") {
        ret = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S32_LE);
    }
    else {
        fprintf(stderr, "unsupported sample format\n");
        exit(EXIT_FAILURE);
    }

    if (ret < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_format: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    ret = snd_pcm_hw_params_set_rate(pcm, params, sampling_rate_hz, 0);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_rate (%d): %s\n", sampling_rate_hz, snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    ret = snd_pcm_hw_params_set_buffer_size(pcm, params, period_size_frames * num_periods);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_buffer_size: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    ret = snd_pcm_hw_params_set_period_size(pcm, params, period_size_frames, 0);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_period_size (%d): %s\n", period_size_frames, snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    ret = snd_pcm_hw_params(pcm, params);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_hw_params: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    // #################### alsa pcm device software params
    snd_pcm_sw_params_t *sw_params;
    snd_pcm_sw_params_alloca(&sw_params);

    ret = snd_pcm_sw_params_current(pcm, sw_params);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_sw_params_current: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }


    ret = snd_pcm_sw_params_set_avail_min(pcm, sw_params, period_size_frames);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_sw_params_set_avail_min: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    // ret = snd_pcm_sw_params_set_start_threshold(pcm, sw_params, 0);
    ret = snd_pcm_sw_params_set_start_threshold(pcm, sw_params, period_size_frames);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_sw_params_set_start_threshold: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    snd_pcm_sw_params(pcm, sw_params);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_sw_params: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    if (verbose) { fprintf(stderr, "done.\n"); }

    return(EXIT_SUCCESS);
}

