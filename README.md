autorustler
===========

Tom's build of Andy's Autorustler.

### To get the raspi cam include files:
git submodule update --init --recursive


### To disable broken dtoverlay:
vi userland/host_applications/linux/CMakeLists.txt

(Comment out dtoverlay)


### To install requirements:
sudo apt-get install libeigen3-dev


### To build:
```
cmake .

make
```
