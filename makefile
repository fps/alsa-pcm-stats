CXXFLAGS ?= -march=native -O3 -Wall -pedantic -pthread -lboost_program_options -lasound

.phony: all

all: alsa-pcm-stats

