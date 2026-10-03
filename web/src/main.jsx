import { createRoot } from 'react-dom/client';
import { DataProvider } from './data.jsx';
import App from './App.jsx';

// styles.css is linked from index.html so the theme paints before any JS runs.
createRoot(document.getElementById('root')).render(<DataProvider><App /></DataProvider>);
