#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>

int setup_pcm_device(snd_pcm_t *pcm, int channels) {
    int ret = 0;

    // #################### alsa pcm device hardware parameters
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    ret = snd_pcm_hw_params_any(pcm, params);

    ret = snd_pcm_hw_params_set_channels(pcm, params, channels);
    if (ret < 0) {
        fprintf(stderr, "Error: snd_pcm_hw_params_set_channels: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    ret = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (ret < 0) {
        fprintf(stderr, "Error: snd_pcm_hw_params_set_access: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    if (sample_format == "S16LE") {
        ret = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    }
    else if (sample_format == "S32LE") {
        ret = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S32_LE);
    }
    else {
        fprintf(stderr, "Error: unsupported sample format\n");
        exit(EXIT_FAILURE);
    }

    if (ret < 0) {
        fprintf(stderr, "Error: snd_pcm_hw_params_set_format: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    ret = snd_pcm_hw_params_set_rate(pcm, params, sampling_rate_hz, 0);
    if (ret < 0) {
        fprintf(stderr, "Error: snd_pcm_hw_params_set_rate (%d): %s\n", sampling_rate_hz, snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    ret = snd_pcm_hw_params_set_buffer_size(pcm, params, period_size_frames * num_periods);
    if (ret < 0) {
        fprintf(stderr, "Error: snd_pcm_hw_params_set_buffer_size: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    ret = snd_pcm_hw_params_set_period_size(pcm, params, period_size_frames, 0);
    if (ret < 0) {
        fprintf(stderr, "Error: snd_pcm_hw_params_set_period_size (%d): %s\n", period_size_frames, snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    ret = snd_pcm_hw_params(pcm, params);
    if (ret < 0) {
        fprintf(stderr, "Error: snd_pcm_hw_params: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    // #################### alsa pcm device software params
    snd_pcm_sw_params_t *sw_params;
    snd_pcm_sw_params_alloca(&sw_params);

    ret = snd_pcm_sw_params_current(pcm, sw_params);
    if (ret < 0) {
        fprintf(stderr, "Error: snd_pcm_sw_params_current: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }


    ret = snd_pcm_sw_params_set_avail_min(pcm, sw_params, period_size_frames);
    if (ret < 0) {
        fprintf(stderr, "Error: snd_pcm_sw_params_set_avail_min: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    // ret = snd_pcm_sw_params_set_start_threshold(pcm, sw_params, 0);
    ret = snd_pcm_sw_params_set_start_threshold(pcm, sw_params, period_size_frames);
    if (ret < 0) {
        fprintf(stderr, "Error: snd_pcm_sw_params_set_start_threshold: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    snd_pcm_sw_params(pcm, sw_params);
    if (ret < 0) {
        fprintf(stderr, "Error: snd_pcm_sw_params: %s\n", snd_strerror(ret));
        exit(EXIT_FAILURE);
    }

    if (verbose) { fprintf(stderr, "Done.\n"); }

    return(EXIT_SUCCESS);
}

