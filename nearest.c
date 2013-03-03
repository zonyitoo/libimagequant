
#include "pam.h"
#include "nearest.h"
#include "mempool.h"
#include <stdlib.h>

struct color_entry {
    f_pixel color;
    unsigned int index;
};

struct sorttmp {
    float radius;
    unsigned int index;
};

struct head {
    f_pixel center;
    float radius;
    unsigned int num_candidates;
    struct color_entry *candidates;
};

struct nearest_map {
    struct head *heads;
    mempool mempool;
    unsigned int num_heads;
};

static int compareradius(const void *ap, const void *bp)
{
    float a = ((const struct sorttmp*)ap)->radius;
    float b = ((const struct sorttmp*)bp)->radius;
    return a > b ? 1 : (a < b ? -1 : 0);
}


const float error_margin = 2.f/256.f;

static struct head build_head(f_pixel px, const colormap *map, unsigned int num_candidates, mempool *m, bool skip_index[], unsigned int *skipped)
{
    struct sorttmp colors[map->colors];
    unsigned int colorsused=0;

    for(unsigned int i=0; i < map->colors; i++) {
        if (skip_index[i]) continue;
        colors[colorsused].index = i;
        colors[colorsused].radius = colordifference(px, map->palette[i].acolor);
        colorsused++;
    }

    qsort(&colors, colorsused, sizeof(colors[0]), compareradius);
    assert(colorsused < 2 || colors[0].radius <= colors[1].radius);

    num_candidates = MIN(colorsused, num_candidates);

    struct head h;
    h.candidates = mempool_new(m, num_candidates * sizeof(h.candidates[0]), 0);
    h.center = px;
    h.num_candidates = num_candidates;
    for(unsigned int i=0; i < num_candidates; i++) {
        h.candidates[i] = (struct color_entry) {
            .color = map->palette[colors[i].index].acolor,
            .index = colors[i].index,
        };
    }
    h.radius = colors[num_candidates-1].radius/4.0f; // /2 squared

    for(unsigned int i=0; i < num_candidates; i++) {

        assert(colors[i].radius <= h.radius*4.0f);
        // divide again as that's matching certain subset within radius-limited subset
        // - 1/256 is a tolerance for miscalculation (seems like colordifference isn't exact)
        if (colors[i].radius < h.radius/4.f - error_margin) {
            skip_index[colors[i].index]=true;
            (*skipped)++;
        }
    }
    return h;
}

static colormap *get_subset_palette(const colormap *map)
{
    if (map->subset_palette) {
        return map->subset_palette;
    }

    unsigned int subset_size = (map->colors+3)/4;
    colormap *subset_palette = pam_colormap(subset_size);

    for(unsigned int i=0; i < subset_size; i++) {
        subset_palette->palette[i] = map->palette[i];
    }

    return subset_palette;
}

struct nearest_map *nearest_init(const colormap *map)
{
    colormap *subset_palette = get_subset_palette(map);

    const unsigned long mempool_size = sizeof(struct color_entry) * subset_palette->colors * map->colors/5 + (1<<14);
    mempool m = NULL;
    struct nearest_map *centroids = mempool_new(&m, sizeof(*centroids), mempool_size);
    centroids->mempool = m;

    unsigned int skipped=0;
    bool skip_index[map->colors]; for(unsigned int j=0; j < map->colors; j++) skip_index[j]=false;


    const unsigned int selected_heads = map->colors > 16 ? MIN(map->colors/4, subset_palette->colors) : 0;
    centroids->heads = mempool_new(&centroids->mempool, sizeof(centroids->heads[0])*(selected_heads+1), mempool_size); // +1 is fallback head

    unsigned int h=0;
    for(; h < selected_heads; h++)
    {
        unsigned int num_candiadtes = 1+(map->colors - skipped)/((1+selected_heads-h)/2);

        centroids->heads[h] = build_head(subset_palette->palette[h].acolor, map, num_candiadtes, &centroids->mempool, skip_index, &skipped);
        if (centroids->heads[h].num_candidates == 0) {
            break;
        }
    }

    centroids->heads[h] = build_head((f_pixel){0,0,0,0}, map, map->colors, &centroids->mempool, skip_index, &skipped);
    centroids->heads[h].radius = MAX_DIFF;
    centroids->num_heads = ++h;

    // get_subset_palette could have created a copy
    if (subset_palette != map->subset_palette) {
        pam_freecolormap(subset_palette);
    }

    return centroids;
}

unsigned int nearest_search(const struct nearest_map *centroids, const f_pixel px, const float min_opaque_val, float *diff)
{
    const bool iebug = px.a > min_opaque_val;

    const struct head *const heads = centroids->heads;
    for(unsigned int i=0; /* last head will always be selected */ ; i++) {
        float headdist = colordifference(px, heads[i].center);

        if (headdist <= heads[i].radius) {
            assert(heads[i].num_candidates);
            unsigned int ind=heads[i].candidates[0].index;
            float dist = colordifference(px, heads[i].candidates[0].color);

            /* penalty for making holes in IE */
            if (iebug && heads[i].candidates[0].color.a < 1) {
                dist += 1.f/1024.f;
            }

            for(unsigned int j=1; j < heads[i].num_candidates; j++) {
                float newdist = colordifference(px, heads[i].candidates[j].color);

                /* penalty for making holes in IE */
                if (iebug && heads[i].candidates[j].color.a < 1) {
                    newdist += 1.f/1024.f;
                }

                if (newdist < dist) {
                    dist = newdist;
                    ind = heads[i].candidates[j].index;
                }
            }
            if (diff) *diff = dist;
            return ind;
        }
    }
}

void nearest_free(struct nearest_map *centroids)
{
    mempool_free(centroids->mempool);
}
