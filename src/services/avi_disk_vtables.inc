#define AVI_FILE_METHODS(family) \
        {family,"QueryInterface",3}, \
        {family,"AddRef",1}, \
        {family,"Release",1}, \
        {family,"Info",3}, \
        {family,"GetStream",4}, \
        {family,"CreateStream",3}, \
        {family,"WriteData",4}, \
        {family,"ReadData",4}, \
        {family,"EndRecord",1}, \
        {family,"DeleteStream",3},
        AVI_FILE_METHODS("internal-avi-file")
        AVI_FILE_METHODS("internal-avi-composition")
#undef AVI_FILE_METHODS
#define AVI_STREAM_METHODS(family) \
        {family,"QueryInterface",3}, \
        {family,"AddRef",1}, \
        {family,"Release",1}, \
        {family,"Create",3}, \
        {family,"Info",3}, \
        {family,"FindSample",3}, \
        {family,"ReadFormat",4}, \
        {family,"SetFormat",4}, \
        {family,"Read",7}, \
        {family,"Write",8}, \
        {family,"Delete",3}, \
        {family,"ReadData",4}, \
        {family,"WriteData",4}, \
        {family,"SetInfo",3},
        AVI_STREAM_METHODS("internal-avi-stream")
        AVI_STREAM_METHODS("internal-avi-editable")
#undef AVI_STREAM_METHODS
        {"internal-avi-edit","QueryInterface",3},
        {"internal-avi-edit","AddRef",1},
        {"internal-avi-edit","Release",1},
        {"internal-avi-edit","Cut",4},
        {"internal-avi-edit","Copy",4},
        {"internal-avi-edit","Paste",6},
        {"internal-avi-edit","Clone",2},
        {"internal-avi-edit","SetInfo",3},
        {"internal-avi-persist","QueryInterface",3},
        {"internal-avi-persist","AddRef",1},
        {"internal-avi-persist","Release",1},
        {"internal-avi-persist","GetClassID",2},
        {"internal-avi-persist","IsDirty",1},
        {"internal-avi-persist","Load",3},
        {"internal-avi-persist","Save",3},
        {"internal-avi-persist","SaveCompleted",2},
        {"internal-avi-persist","GetCurFile",2},
        {"internal-avi-factory","QueryInterface",3},
        {"internal-avi-factory","AddRef",1},
        {"internal-avi-factory","Release",1},
        {"internal-avi-factory","CreateInstance",4},
        {"internal-avi-factory","LockServer",2},
