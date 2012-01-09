void update_entry(char *path,int when,response_t what,char *etag);
int find_entry(char *path,response_t *what,int *timestamp, char *etag,int etaglen);
void debug_entries(void);