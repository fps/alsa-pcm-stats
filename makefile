CXXFLAGS ?= -march=native -O3 -g -Wall -pthread -lboost_program_options -lasound

.phony: all

all: alsa-pcm-stats

