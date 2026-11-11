// Link-closure probe: the whole of src/ behind the game's own entry point.
extern "C" void NitroMain(void);
int main(void) { NitroMain(); return 0; }
