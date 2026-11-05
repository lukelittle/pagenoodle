// Imported first by workers so the network guard is in place before any library code runs.
import { installNetGuard } from './netguard';
installNetGuard();
