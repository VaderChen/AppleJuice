constexpr WindowsServiceDescriptor fileMethods[]{
{"internal-d3d9","XFile.QueryInterface",3},{"internal-d3d9","XFile.AddRef",1},{"internal-d3d9","XFile.Release",1},
{"internal-d3d9","XFile.CreateEnumObject",4},{"internal-d3d9","XFile.CreateSaveObject",5},
{"internal-d3d9","XFile.RegisterTemplates",3},{"internal-d3d9","XFile.RegisterEnumTemplates",2}};
constexpr WindowsServiceDescriptor fileEnumMethods[]{
{"internal-d3d9","XEnum.QueryInterface",3},{"internal-d3d9","XEnum.AddRef",1},{"internal-d3d9","XEnum.Release",1},
{"internal-d3d9","XEnum.GetFile",2},{"internal-d3d9","XEnum.GetChildren",2},{"internal-d3d9","XEnum.GetChild",3},
{"internal-d3d9","XEnum.GetDataObjectById",3},{"internal-d3d9","XEnum.GetDataObjectByName",3}};
constexpr WindowsServiceDescriptor fileDataMethods[]{
{"internal-d3d9","XData.QueryInterface",3},{"internal-d3d9","XData.AddRef",1},{"internal-d3d9","XData.Release",1},
{"internal-d3d9","XData.GetEnum",2},{"internal-d3d9","XData.GetName",3},{"internal-d3d9","XData.GetId",2},
{"internal-d3d9","XData.Lock",3},{"internal-d3d9","XData.Unlock",1},{"internal-d3d9","XData.GetType",2},
{"internal-d3d9","XData.IsReference",1},{"internal-d3d9","XData.GetChildren",2},{"internal-d3d9","XData.GetChild",3}};
