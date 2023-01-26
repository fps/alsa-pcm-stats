for n in $(seq 1 10); 
  do for period in 6 12 16 24 32 48 64 96 128 192 256 512 1024 2048
    do for nperiods in 2 3; do echo $nperiods $period
       ./alsa-pcm-stats -d hw:iXR  -w 0 -a 1 -n $nperiods -p $period -s $(echo "48000 / $period" | bc) | tee > samples_nperiods_"$nperiods"_periodsize_"$period"_index_"$n".txt
    done
  done
done
