#include <stdio.h>
#include <string.h>

#define MAX_FLOOR 20
#define MIN_FLOOR 1

typedef enum
{
    UP = 1,
    DOWN = -1,
    IDLE = 0
} Direction;

typedef struct
{
    int floor;
    Direction dir;
    int req[MAX_FLOOR + 1];
} Elevator;

void init(Elevator *e);

void add(Elevator *e, int f);

int find(Elevator *e);

void set_dir(Elevator *e);

void move(Elevator *e);

void run(Elevator *e);
