import json, pathlib, statistics, sys
root=pathlib.Path(sys.argv[1]) if len(sys.argv)>1 else pathlib.Path(__file__).parent

def read(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.startswith('{"name":')]

def aggregate(paths,fields,invariants):
    runs=[read(p) for p in paths]
    names=[r['name'] for r in runs[0]]
    assert names and len(names)==len(set(names)), paths
    assert all([r['name'] for r in run]==names for run in runs), paths
    out=[]
    for rows in zip(*runs):
        result={'name':rows[0]['name'],'runs':len(rows)}
        for field in invariants:
            values=[r.get(field) for r in rows]
            assert all(v==values[0] for v in values), (result['name'],field,values)
            result[field]=values[0]
        for field in fields:
            values=[r[field] for r in rows if r.get(field) is not None]
            if values:
                result[field]={'median':statistics.median(values),'min':min(values),'max':max(values)}
        out.append(result)
    return out

public_paths=[root/'public-1.jsonl']+[root/f'public-{n}.log' for n in range(2,6)]
summary={'public':aggregate(public_paths,['mean_us','p50_us','p95_us','p99_us','max_us','process_rss_kib'],['samples','visible_bytes','history_edits','wire_bytes']),
'native':aggregate([root/f'native-{n}.log' for n in range(1,6)],['wall_ns','ns_per_work_item','work_per_sec','process_rss_kb','process_rss_delta_kb'],['work_items','work_label','source_edits','remote_applications','fragments','tombstones','visible_bytes','deleted_bytes'])}
(root/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
for group,rows in summary.items():
    print(group,len(rows))
    for row in rows:
        if group=='public':
            print(row['name'],*[f'{row[f]["median"]:.3f}' for f in ['mean_us','p50_us','p99_us']], 'p99range',row['p99_us']['min'],row['p99_us']['max'])
        else: print(row['name'], round(row['ns_per_work_item']['median']/1000,2), 'us/work',row['work_items'],row['fragments'],row['tombstones'])
