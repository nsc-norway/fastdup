#!/usr/bin/env bash

single_read=false
supr_args=()
while [[ ! -z "$1" ]];
do
	if [[ $1 == "-1" ]]
	then
		single_read=true
	elif [[ $1 == -* ]]
	then
		supr_args+=($1)
		shift
	else
		break
	fi
done

if $single_read
then
	r1file="$1"
	r1output="$2"
else
	r1file="$1"
	r1output="$3"
	r2file="$2"
	r2output="$4"
fi

if [[ ! -f "$r1file" ]]
then
	echo "Error: '$r1file' is not a file."
	exit 1
fi

if [[ -z "$1" ]]
then
	echo ""
	echo "usage: filter-pe.sh [-1] [SUPR_OPTIONS] R1_FILE [R2_FILE] R1_OUTPUT [R2_OUTPUT]"
	echo ""
	echo "       Use option -1 to process single-read data."
	echo ""
	exit 1
fi

if $single_read;
then
	./suprDUPr.read_id ${supr_args[*]} "$r1file" | ./filter-dups.pl "$r1file" > "$r1output"
else
	read_id_fifo=`mktemp -u`_readidfifo
	mkfifo $read_id_fifo
	./suprDUPr.read_id ${supr_args[*]} "$r1file" | tee $read_id_fifo | ./filterfq "$r1file" > "$r1output" &
	./filterfq "$r2file" > "$r2output" < $read_id_fifo &
	wait
	rm $read_id_fifo
fi
