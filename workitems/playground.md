
**Protect Linux server from Playground-run code**
```
sudo cmake -DCMAKE_C_FLAGS="-fuse-ld=lld" -DCMAKE_CXX_FLAGS="-fuse-ld=lld" .
bubblewrap
```
