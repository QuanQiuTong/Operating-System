#include<stdlib.h>

int main(){
    system("cd ../build && make clean");

    system("timeout 1s make qemu > proc.txt");
    
    system("grep 'PASS' proc.txt");
}