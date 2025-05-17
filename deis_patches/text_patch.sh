#!/bin/bash

clear

if ! which gsed >/dev/null ; then echo "Needs gsed to work. Exiting." ; exit ; fi

for file in $(find ../ -type f) ; do
 gsed -i "s/Bitcoin Deis/Bitcoin Deis/g" $file
done


for file in $(find ../ -type f) ; do 
if ! grep -q "Please contribute if you find Bitcoin Deis useful. Visit" $file ; then continue ; fi
 gsed -i 's|github\.com/bitcoin/bitcoin|github\.com/armantheparman/deis|g' $file 
done

