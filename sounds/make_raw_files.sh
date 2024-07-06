#!/bin/bash

infile=${1:-ikea.mp3}
outfilepath=${infile%.*}
outfile=${outfilepath##*/}

ffmpeg -i "$infile" -ar:a 16000 -filter:a "volume=0.1" -acodec pcm_s16le -map_channel 0.0.0:0.0 -f s16le "${outfile}_left.s16le"
ffmpeg -i "$infile" -ar:a 16000 -filter:a "volume=0.1" -acodec pcm_s16le -map_channel 0.0.1:0.0 -f s16le "${outfile}_right.s16le"
