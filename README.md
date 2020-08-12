# L-SMASH-Works

work base on L-SMASH-Works

remove "InputFilePath" section in the index file, so the filter won't read the file again if the path was changed.

# Compilation

```
apt-get install meson ninja
meson build
ninja -C build
ninja -C build install
```

if there is an ld error
> compile l-smash with --enable-shared flag.
