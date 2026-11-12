"""Regenerate the embedded payload after editing locations.json."""
from pathlib import Path
import argparse, hashlib, importlib.util, json
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--game',required=True,type=Path,help='The supported FH3 Mount folder')
a=p.parse_args()
spec=importlib.util.spec_from_file_location('asset_builder',ROOT/'tools/asset_builder.py')
b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
game=a.game/'Mount' if (a.game/'Mount').is_dir() else a.game
b.verify_build(game)
sites=json.loads((ROOT/'locations.json').read_text(encoding='utf-8-sig'));b.validate_sites(sites)
files=b.source_files(game/'media',sites);payload=b.pack(sites,files,game/'media')
lines=['// Generated Car Meets data.\n#pragma once\ninline constexpr unsigned char car_meets_payload[] = {\n']
for i in range(0,len(payload),24):lines.append('    '+','.join('0x%02x'%x for x in payload[i:i+24])+',\n')
lines.append('};\n');(ROOT/'src/embedded_payload.h').write_text(''.join(lines))
(ROOT/'payload-manifest.json').write_text(json.dumps({'format':'CML9','bytes':len(payload),'sha256':hashlib.sha256(payload).hexdigest(),'added_meets':len(sites),'localized_labels':len(sites)*2,'overrides':len(files)},indent=2)+'\n')
print('Embedded',len(payload),'bytes;',len(sites),'meets;',len(files),'overrides.')
