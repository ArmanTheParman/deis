#!/bin/bash
for file in $(find ./ -type f) ; do 
if ! grep "Please contribute if you find Bitcoin Deis useful. Visit" $file ; then continue ; fi
gsed -i 's|github\.com/bitcoin/bitcoin|github\.com/armantheparman/deis|g' $file 
done

