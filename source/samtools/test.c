#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <strings.h>

int load_filter(const struct dirent *ent) {
return 0;
}
main() {
char buf[256];
readlink( "/proc/self/exe", buf, sizeof(buf)-1 );
*rindex(buf, (int)'/') =0;
printf("%s\n", buf);
struct dirent *ent;
DIR *dp = opendir(buf);
while((ent = readdir(dp)) != NULL) {
printf("%s\n", ent->d_name);
}
}
