#include "main.h"

void init(Elevator *e) 
{
    e->floor = 1;
    e->dir = IDLE;
    memset(e->req, 0, sizeof(e->req));//内存填充函数
}

void add(Elevator *e, int f) 
{
    if (f < 1 || f > MAX_FLOOR || f == e->floor) 
    return;
    e->req[f] = 1;
    printf("  [%d楼] 请求加入\n", f);
}

int find(Elevator *e) 
{
    for (int i = 1; i <= MAX_FLOOR; i++)
        if (e->req[i]) 
        return 1;
    return 0;
}

void set_dir(Elevator *e)
{
    if (e->dir == UP) {
        for (int i = e->floor + 1; i <= MAX_FLOOR; i++)
            if (e->req[i]) 
            return;
    } else if (e->dir == DOWN) {
        for (int i = e->floor - 1; i >= 1; i--)
            if (e->req[i]) 
            return;
    }
    int up = 999, down = 999;           
    for (int i = e->floor + 1; i <= MAX_FLOOR; i++)
        if (e->req[i]) { 
            up = i - e->floor; 
            break; 
        }
    for (int i = e->floor - 1; i >= 1; i--)
        if (e->req[i]) { 
            down = e->floor - i; 
            break; 
        }
    if (up == 999 && down == 999)       
        e->dir = IDLE;
    else if (up <= down)
        e->dir = UP;
    else
        e->dir = DOWN;
}

void move(Elevator *e)
{
    if (e->dir == UP && e->floor < MAX_FLOOR) e->floor++;
    else if (e->dir == DOWN && e->floor > 1) e->floor--;
}

void run(Elevator *e)
{
    printf("\n--- LOOK 调度启动 ---\n");
    while (find(e) || e->dir != IDLE) {
        if (e->req[e->floor]) {
            printf("  => %d楼 到达，开门\n", e->floor);
            e->req[e->floor] = 0;
        }
        set_dir(e);
        if (e->dir == IDLE) { 
            printf("  [全部完成，电梯空闲]\n");
            break;
        }
        move(e);
        if (e->dir == UP)
        printf("  上升 -> %d楼\n", e->floor);
        else
        printf("  下降 -> %d楼\n", e->floor);
    }
}

int main(int argc, char const *argv[])
{
    printf("=== 单电梯 LOOK 调度 ===\n");
    Elevator e;
    init(&e);

    printf("\n[第一轮] 电梯在1楼,请求: 8,3,12,5,15,2,10\n");
    add(&e, 8);
    add(&e, 3);
    add(&e, 12);
    add(&e, 5);
    add(&e, 15);
    add(&e, 2);
    add(&e, 10);
    run(&e);

    printf("\n[第二轮] 电梯在%d楼,请求: 6,18,1,9\n", e.floor);
    add(&e, 6);
    add(&e, 18);
    add(&e, 1);
    add(&e, 9);
    run(&e);

    printf("\n=== 调度结束 ===\n");
    return 0;
}
