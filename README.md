# Bitcoin Deis
=====================================

To build Bitcoin Deis, either install it using Parmanode (make a selection for Deis when prompted during Bitcoin installation), or
clone Bitcoin core (a fork provided here in this repo), and modify before compiling (with GUI).

The relevent BASH code in Parmanode is (run just before compiling, after cloning repository):

deis="true" #selection flag in earlier menus

export hp=$HOME/parmanode

export pn=$HOME/parman_programs/parmanode

git clone https://github.com/bitcoin/bitcoin $hp/bitcon_github

#alternatively, you can clone from this fork

#git clone https://github.com/armantheparman/deis $hp/bitcoin_github

if [[ $deis == "true" ]] ; then

cd $hp/bitcoin_github 

git checkout v28.1

cp $pn/src/deis/icons/deis.png $hp/bitcoin_github/src/qt/res/icons/bitcoin.png

cp $pn/src/deis/icons/deis.ico $hp/bitcoin_github/src/qt/res/icons/bitcoin.ico

cp $pn/src/deis/deis.svg $hp/bitcoin_github/src/qt/res/src/bitcoin.svg

cp $pn/src/deis/share/icons/pixmaps/bitcoin64.png $hp/bitcoin_github/doc/bitcoin_logo_doxygen.png

rm $hp/bitcoin_github/share/pixmaps/*

cp $pn/src/deis/share/icons/pixmaps/* $hp/bitcoin_github/share/pixmaps/*

    for file in $(find $hp/bitcoin_github -type f) ; do
        gsed -i "s/Bitcoin Core/Bitcoin Deis/g" $file
    
    done
    
fi

Now that the image files and the relevant text strings have been modified in the source code, you can compile as normal. Set --with-gui="yes" to also compile QT.

More information on why you should run a node:

https://armantheparman.com/6reasonsnode
