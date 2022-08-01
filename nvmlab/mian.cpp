#include "mian.h"

#include <fstream>
#include <iostream>
#include <libpmemobj.h>
#include <map>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

#define MAX_BUF_LEN 100

bool do_not_dump = false;

struct kv{
    char k[key_length + 1];
    char v[value_length + 1];
};

struct my_root
{
    int len;
    kv my_state[MAX_BUF_LEN];
};

std::map<std::string, std::string> state;

static inline int file_exists(char const *file) { return access(file, F_OK); }

void exit_func()
{
    if (!do_not_dump)
    {
        FILE *fp = fopen("/mnt/pmem1", "wb");

        int size = state.size();
        fwrite(&size, sizeof(size), 1, fp);

        for (auto &[k, v] : state)
        {
            fwrite(k.c_str(), key_length, 1, fp);
            fwrite(v.c_str(), value_length, 1, fp);
        }

        fclose(fp);
    }
}

void mian(std::vector<std::string> args)
{
    atexit(&exit_func);

    auto filename = args[0].c_str();

    PMEMobjpool *pop;

    if (file_exists(filename) != 0)
    {
        //PMEMOBJ_MIN_POOL
        pop = pmemobj_create(filename, "QAQ", 150L<<20, 0666);
    }
    else
    {
        pop = pmemobj_open(filename, "QAQ");

        PMEMoid r_root = pmemobj_root(pop, sizeof(struct my_root));
        struct my_root *r_rootp = (struct my_root *)pmemobj_direct(r_root);

        for (int i = 0; i < r_rootp->len; i++)
        {
            char k[key_length + 1] = {0}, v[value_length + 1] = {0};
            
            strcpy(k, r_rootp->my_state[i].k);
            strcpy(v, r_rootp->my_state[i].v);

            state[k] = v;
        }

        do_not_dump = true;
    }

    if (pop == NULL)
    {
        std::cout << filename << std::endl;
        perror("pmemobj_create");
        return;
    }

    PMEMoid root = pmemobj_root(pop, sizeof(struct my_root));
    struct my_root *rootp = (struct my_root *)pmemobj_direct(root);

    char buf[MAX_BUF_LEN] = "114514";

    while (1)
    {
        Query q = nextQuery();

        switch (q.type)
        {
        case Query::SET:
            TX_BEGIN(pop){
                kv qdata;
                strcpy(qdata.k, q.key.c_str());
                strcpy(qdata.v, q.value.c_str());
                rootp->len++;
                pmemobj_persist(pop, &rootp->len, sizeof(rootp->len));
                pmemobj_memcpy_persist(pop, rootp->my_state + (rootp->len - 1), &qdata, sizeof(qdata));
            }TX_END

            break;

        case Query::GET:
            if (state.count(q.key))
                q.callback(state[q.key]);
            else
                q.callback("-");
            break;

        case Query::NEXT:
            if (auto it = state.upper_bound(q.key); it != state.end())
                q.callback(it->first);
            else
                q.callback("-");
            break;

        default:
            throw std::invalid_argument(std::to_string(q.type));
        }

        
    }


    pmemobj_close(pop);
}