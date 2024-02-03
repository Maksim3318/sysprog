# hw1

### Build

```
make
```

### Generate test files

```
python3 generator.py -f filename -c count -m max
```

For test:
```
python3 generator.py -f test1.txt -c 40000 -m 1000000 && python3 generator.py -f test2.txt -c 40000 -m 1000000 && python3 generator.py -f test3.txt -c 40000 -m 1000000 && python3 generator.py -f test4.txt -c 40000 -m 1000000 && python3 generator.py -f test5.txt -c 40000 -m 1000000 && python3 generator.py -f test6.txt -c 40000 -m 1000000
```

### Run

```
./main T N files_list
```
T - target latency

N - coroutine count

For test:
```
HHREPORT=v ./main 100 4 test1.txt test2.txt test3.txt test4.txt test5.txt test6.txt
```

### Check result

```
[ -s out.txt ] && python3 checker.py -f out.txt
```