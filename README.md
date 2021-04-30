# uring-sendfile
Just a simple implementation of sendfile using IO_URING. Hopefully it can help someone searching for hints on how to do this.

## How to use

### Starting server
```sh
make && ./server file.txt
```

### Make a request
```sh
curl -o curled.txt http://localhost:6000
```

### Verify
```sh
diff curled.txt file.txt
```


