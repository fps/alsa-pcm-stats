CXXFLAGS ?= -march=native -O3 -Wall -Wextra -pedantic -pthread -lboost_program_options -lasound

.phony: all

all: alsa-pcm-stats-busy-wait alsa-pcm-stats-poll

