//! PC metrics sent to the screen every second (see protocol/README.md, "metrics").

use serde_json::{json, Map, Value};
use std::time::Instant;
use sysinfo::{Components, Disks, Networks, System};

/// Keeps the sysinfo handles between samples: CPU usage and network rates are deltas.
pub struct Collector {
    system: System,
    networks: Networks,
    disks: Disks,
    components: Components,
    last_sample: Instant,
    gpu: gpu::Reader,
}

impl Collector {
    pub fn new() -> Self {
        let mut system = System::new();
        system.refresh_cpu_usage();
        system.refresh_memory();
        Self {
            system,
            networks: Networks::new_with_refreshed_list(),
            disks: Disks::new_with_refreshed_list(),
            components: Components::new_with_refreshed_list(),
            last_sample: Instant::now(),
            gpu: gpu::Reader::new(),
        }
    }

    /// One "metrics" message. Fields the machine cannot provide are left out.
    pub fn sample(&mut self) -> Value {
        let elapsed = self.last_sample.elapsed().as_secs_f64().max(0.001);
        self.last_sample = Instant::now();

        self.system.refresh_cpu_usage();
        self.system.refresh_memory();
        self.networks.refresh(true);
        self.disks.refresh(true);
        self.components.refresh(true);

        let mut m = Map::new();
        m.insert("type".into(), json!("metrics"));
        m.insert("cpu".into(), json!(round1(self.system.global_cpu_usage() as f64)));

        let total = self.system.total_memory();
        let used = self.system.used_memory();
        if total > 0 {
            m.insert("ram".into(), json!(round1(used as f64 * 100.0 / total as f64)));
            m.insert("ram_used".into(), json!(used));
            m.insert("ram_total".into(), json!(total));
        }

        // Network: bytes since the previous refresh, over every interface except loopback
        let (mut rx, mut tx) = (0u64, 0u64);
        for (name, data) in self.networks.iter() {
            if name.starts_with("lo") {
                continue;
            }
            rx += data.received();
            tx += data.transmitted();
        }
        m.insert("net_rx".into(), json!((rx as f64 / elapsed).round()));
        m.insert("net_tx".into(), json!((tx as f64 / elapsed).round()));

        if let Some(disk) = system_disk(&self.disks) {
            m.insert("disk".into(), json!(disk));
        }
        if let Some(temp) = cpu_temperature(&self.components) {
            m.insert("cpu_temp".into(), json!(temp.round()));
        }

        let gpu = self.gpu.read();
        if let Some(usage) = gpu.usage {
            m.insert("gpu".into(), json!(round1(usage)));
        }
        if let Some(temp) = gpu.temperature {
            m.insert("gpu_temp".into(), json!(temp.round()));
        }
        if let Some(vram) = gpu.memory {
            m.insert("vram".into(), json!(round1(vram)));
        }
        Value::Object(m)
    }
}

fn round1(value: f64) -> f64 {
    (value * 10.0).round() / 10.0
}

/// Usage of the system disk, in percent. On macOS the writable data volume holds the user's files.
fn system_disk(disks: &Disks) -> Option<f64> {
    let preferred = ["/System/Volumes/Data", "/", "C:\\"];
    preferred.iter().find_map(|mount| {
        disks.iter().find(|d| d.mount_point().to_string_lossy() == *mount).and_then(|d| {
            let total = d.total_space();
            (total > 0).then(|| round1((total - d.available_space()) as f64 * 100.0 / total as f64))
        })
    })
}

/// Hottest CPU sensor. Sensor names differ a lot between machines, so this is a best effort.
fn cpu_temperature(components: &Components) -> Option<f64> {
    components
        .iter()
        .filter(|c| {
            let label = c.label().to_lowercase();
            label.contains("cpu") || label.contains("tdie") || label.contains("package") || label.contains("core")
        })
        .filter_map(|c| c.temperature())
        .map(|t| t as f64)
        .filter(|t| t.is_finite() && *t > 0.0 && *t < 130.0)
        .reduce(f64::max)
}

/// GPU readings, each one optional.
#[derive(Default)]
pub struct GpuReading {
    pub usage: Option<f64>,
    pub temperature: Option<f64>,
    pub memory: Option<f64>,
}

#[cfg(target_os = "macos")]
mod gpu {
    use super::GpuReading;
    use std::process::Command;

    /// macOS: "Device Utilization %" of the IOAccelerator driver statistics (Apple Silicon and most Macs).
    pub struct Reader;

    impl Reader {
        pub fn new() -> Self {
            Reader
        }

        pub fn read(&mut self) -> GpuReading {
            let output = Command::new("/usr/sbin/ioreg").args(["-r", "-d", "1", "-w", "0", "-c", "IOAccelerator"]).output();
            let text = match output {
                Ok(out) => String::from_utf8_lossy(&out.stdout).into_owned(),
                Err(_) => return GpuReading::default(),
            };
            GpuReading { usage: find_number(&text, "\"Device Utilization %\"="), ..Default::default() }
        }
    }

    fn find_number(text: &str, key: &str) -> Option<f64> {
        let start = text.find(key)? + key.len();
        let digits: String = text[start..].chars().take_while(|c| c.is_ascii_digit()).collect();
        digits.parse().ok()
    }
}

#[cfg(windows)]
mod gpu {
    use super::GpuReading;
    use nvml_wrapper::enum_wrappers::device::TemperatureSensor;
    use nvml_wrapper::Nvml;

    /// Windows: NVIDIA cards through NVML. Other GPUs report nothing.
    pub struct Reader {
        nvml: Option<Nvml>,
    }

    impl Reader {
        pub fn new() -> Self {
            Reader { nvml: Nvml::init().ok() }
        }

        pub fn read(&mut self) -> GpuReading {
            let Some(nvml) = &self.nvml else { return GpuReading::default() };
            let Ok(device) = nvml.device_by_index(0) else { return GpuReading::default() };
            GpuReading {
                usage: device.utilization_rates().ok().map(|u| u.gpu as f64),
                temperature: device.temperature(TemperatureSensor::Gpu).ok().map(|t| t as f64),
                memory: device
                    .memory_info()
                    .ok()
                    .filter(|m| m.total > 0)
                    .map(|m| m.used as f64 * 100.0 / m.total as f64),
            }
        }
    }
}

#[cfg(not(any(target_os = "macos", windows)))]
mod gpu {
    use super::GpuReading;

    pub struct Reader;

    impl Reader {
        pub fn new() -> Self {
            Reader
        }

        pub fn read(&mut self) -> GpuReading {
            GpuReading::default()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sample_has_the_basic_fields() {
        let mut collector = Collector::new();
        std::thread::sleep(std::time::Duration::from_millis(300));
        let m = collector.sample();
        println!("{m}");
        assert_eq!(m["type"], "metrics");
        assert!(m["cpu"].is_number());
        assert!(m["ram"].as_f64().unwrap() > 0.0);
        assert!(m["ram_total"].as_u64().unwrap() > 0);
        assert!(m["net_rx"].is_number());
    }
}
