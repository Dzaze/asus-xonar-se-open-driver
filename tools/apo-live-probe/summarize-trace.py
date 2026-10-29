"""Condense tracerpt XML without loading a large audio trace into memory."""
import json
import sys
import xml.etree.ElementTree as ET

NS = {"e": "http://schemas.microsoft.com/win/2004/08/events/event"}
APO = "{6a636141-b93c-40c6-b934-4a4c0848fb29}"
report = {"eventsLost": [], "imageLoads": [], "lifecycle": []}
for _, event in ET.iterparse(sys.argv[1], events=("end",)):
    if event.tag != "{" + NS["e"] + "}Event":
        continue
    provider = event.find("e:System/e:Provider", NS)
    execution = event.find("e:System/e:Execution", NS)
    time = event.find("e:System/e:TimeCreated", NS)
    data = {v.get("Name"): v.text or "" for v in event.findall("e:EventData/e:Data", NS)}
    row = {"pid": execution.get("ProcessID") if execution is not None else None,
           "time": time.get("SystemTime") if time is not None else None}
    if "EventsLost" in data:
        report["eventsLost"].append(int(data["EventsLost"]))
    if any("xonarapo.dll" in value.lower() for value in data.values()):
        report["imageLoads"].append(dict(row, data=data))
    if provider is not None and provider.get("Guid", "").lower() == APO:
        message = data.get("Message", "")
        if message.startswith(("DllGetClassObject", "CreateInstance", "Initialize ",
                               "LockForProcess ", "UnlockForProcess ", "Construct", "Destruct")):
            report["lifecycle"].append(dict(row, **data))
    event.clear()
json.dump(report, sys.stdout, indent=2, ensure_ascii=False)
print()
