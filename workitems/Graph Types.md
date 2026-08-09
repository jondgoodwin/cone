



## Subscribe & Publish

Workflow:  Spreadsheets, Responsive compiler, Asset pipeline

### Build/asset pipeline example
pgm A -> Obj A -> .exe
pgm B -> Obj B  /

Exe knows it depends on objA and ObjB, and ditto on source pgms

When pgm B is changed, it uses dependency graph to invalidate objB and Exe to be rebuilt

### Spreadsheet example